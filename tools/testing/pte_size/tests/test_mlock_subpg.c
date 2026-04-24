/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Regression test for the populate_vma_page_range() sub-PG stall.
 *
 * When PG_SIZE > PTE_SIZE, do_mmap() aligns len to PTE_SIZE (not PG_SIZE),
 * so any sub-PG mmap creates a VMA whose extent is smaller than one PG.
 * If such a VMA is VM_LOCKED (e.g. via mlockall(MCL_FUTURE) or MAP_LOCKED),
 * populate_vma_page_range() used to compute nr_pages = (end - start) /
 * PG_SIZE with integer division, yielding 0 for sub-PG ranges.
 * __get_user_pages(0) returned 0 immediately; __mm_populate()'s
 * nend = nstart + 0 * PG_SIZE was a no-op, so the populate loop spun
 * forever in-kernel — exactly the "ata_id worker freezes after exec"
 * symptom that blocked 16K/64K boot for weeks.
 *
 * Fixed by nr_pages = DIV_ROUND_UP(end - start, PG_SIZE).
 *
 * This test mirrors two triggers:
 *   1. mlockall(MCL_FUTURE) + mmap(sub-PG)
 *   2. mmap(sub-PG, MAP_LOCKED)
 * Either would spin forever on the unfixed kernel. A 5-second timeout
 * (SIGALRM) converts a hang into a test failure.
 */
#include "test_common.h"
#include <sys/resource.h>

static volatile sig_atomic_t alarm_fired;

static void on_alarm(int sig)
{
	(void)sig;
	alarm_fired = 1;
}

/* Arm a watchdog; any mmap call that spins in __mm_populate will be
 * interrupted by the signal. The mmap itself can't be killed from inside
 * the kernel's loop, but in a child process the signal will at least
 * cause the child to terminate once it returns to userspace. We wrap each
 * risky call in a fork() so the parent stays alive. */
static int check_no_stall(int (*fn)(void))
{
	pid_t pid = fork();
	int status;

	if (pid == 0) {
		alarm(5);
		signal(SIGALRM, on_alarm);
		_exit(fn());
	}

	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status)) {
		TEST_FAIL("child killed by signal %d (stall?)",
			  WTERMSIG(status));
	}
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	       "child exit %d", WEXITSTATUS(status));
	return 0;
}

static int do_mlockall_then_mmap(void)
{
	void *p;
	size_t len = 4096;	/* sub-PG on any PG >= 8K */
	struct rlimit rl = { .rlim_cur = 64 * 1024 * 1024,
			     .rlim_max = 64 * 1024 * 1024 };

	setrlimit(RLIMIT_MEMLOCK, &rl);

	if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
		/* EPERM is fine — test still exercises the mmap path if we
		 * fall through to MAP_LOCKED below. Skip this sub-case. */
		if (errno == EPERM)
			return 0;
		fprintf(stderr, "  mlockall: %s\n", strerror(errno));
		return 1;
	}

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		fprintf(stderr, "  mmap: %s\n", strerror(errno));
		munlockall();
		return 1;
	}

	((char *)p)[0] = 'A';
	munmap(p, len);
	munlockall();
	return 0;
}

static int do_map_locked(void)
{
	void *p;
	size_t len = 4096;
	struct rlimit rl = { .rlim_cur = 64 * 1024 * 1024,
			     .rlim_max = 64 * 1024 * 1024 };

	setrlimit(RLIMIT_MEMLOCK, &rl);

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
	if (p == MAP_FAILED) {
		if (errno == EPERM || errno == ENOMEM)
			return 0;
		fprintf(stderr, "  mmap(MAP_LOCKED): %s\n", strerror(errno));
		return 1;
	}

	((char *)p)[0] = 'B';
	munmap(p, len);
	return 0;
}

int main(void)
{
	int ret = 0;

	TEST_LOG("mlockall(MCL_FUTURE) + mmap(4K) — regression for populate_vma_page_range sub-PG loop");
	ret |= check_no_stall(do_mlockall_then_mmap);

	TEST_LOG("mmap(4K, MAP_LOCKED)");
	ret |= check_no_stall(do_map_locked);

	return ret ? 1 : 0;
}
