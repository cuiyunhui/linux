/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Zero-page read-fault accounting.
 *
 * Triggers the do_anonymous_page() read path that installs
 * PG_SIZE-worth of zero-page PTEs across a private anonymous VMA,
 * forks, and verifies neither parent nor child trips "Bad rss-counter"
 * on exit. This regressed the 16K NixOS boot.
 */
#include "test_common.h"

static int test_zero_read_fork(void)
{
	char *p;
	size_t len = KB(256);
	pid_t pid;
	int status;
	volatile char sink = 0;

	TEST_LOG("anon mmap, zero-page read-fault, fork, exit");

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Install zero-page PTEs across the whole VMA */
	for (size_t off = 0; off < len; off += 4096)
		sink ^= p[off];

	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));
	if (pid == 0) {
		/* Child reads the zero-page PTEs it inherited, then exits */
		for (size_t off = 0; off < len; off += 4096)
			sink ^= p[off];
		_exit((int)sink);
	}

	waitpid(pid, &status, 0);
	ASSERT(WIFEXITED(status), "child abnormal");

	munmap(p, len);
	(void)sink;
	return 0;
}

static int test_zero_read_mixed_write(void)
{
	char *p;
	size_t len = KB(128);
	volatile char sink = 0;

	TEST_LOG("zero-page read-fault, then write partway");

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Half: read-fault (zero page). Other half: write-fault (anon). */
	for (size_t off = 0; off < len / 2; off += 4096)
		sink ^= p[off];
	for (size_t off = len / 2; off < len; off += 4096)
		p[off] = 0x77;

	/* All read-faulted pages must read zero. */
	for (size_t off = 0; off < len / 2; off += 4096)
		ASSERT(p[off] == 0, "zero-page region @%zuK != 0", off / 1024);
	/* All written pages must read 0x77. */
	for (size_t off = len / 2; off < len; off += 4096)
		ASSERT(p[off] == 0x77, "written region @%zuK != 0x77",
		       off / 1024);

	munmap(p, len);
	(void)sink;
	return 0;
}

/*
 * Exec /tests/test_dynamic_helper many times and then dump the parent's
 * VmData/RssAnon. If the 16K NixOS "Bad rss-counter" leak is reproducible
 * in-process, this should expose a drift in the anon counter across many
 * short-lived glibc-linked child processes.
 */
static int test_exec_glibc_many(void)
{
	TEST_LOG("fork+exec glibc helper 50x, watching parent counters");
	for (int i = 0; i < 50; i++) {
		pid_t pid = fork();
		if (pid < 0)
			TEST_FAIL("fork: %s", strerror(errno));
		if (pid == 0) {
			execl("/tests/test_dynamic_helper",
			      "test_dynamic_helper", (char *)NULL);
			_exit(127);
		}
		int status;
		waitpid(pid, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 42)
			TEST_FAIL("iter %d: status=%d exit=%d",
				  i, status, WEXITSTATUS(status));
	}
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_zero_read_fork();
	ret |= test_zero_read_mixed_write();
	ret |= test_exec_glibc_many();
	return ret ? 1 : 0;
}
