/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test mprotect and permission-change faults.
 *
 * Exercises change_protection() and the resulting faults when
 * accessing memory after permission changes. With PTE_SIZE != PG_SIZE,
 * mprotect boundaries may not align with PG_SIZE.
 */
#include "test_common.h"
#include <setjmp.h>

static sigjmp_buf jmp_env;
static volatile int got_signal;

static void sigsegv_handler(int sig)
{
	got_signal = sig;
	siglongjmp(jmp_env, 1);
}

static int test_mprotect_rw_to_ro(void)
{
	char *p;
	size_t len = KB(64);
	struct sigaction sa, old_sa;

	TEST_LOG("mprotect RW -> RO, verify SIGSEGV on write");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	p[0] = 'A';
	ASSERT(p[0] == 'A', "initial write");

	int ret = mprotect(p, len, PROT_READ);
	ASSERT(ret == 0, "mprotect: %s", strerror(errno));

	/* Read should still work */
	ASSERT(p[0] == 'A', "read after mprotect");

	/* Write should SIGSEGV */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigsegv_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGSEGV, &sa, &old_sa);

	got_signal = 0;
	if (sigsetjmp(jmp_env, 1) == 0)
		p[0] = 'B'; /* Should fault */

	sigaction(SIGSEGV, &old_sa, NULL);
	ASSERT(got_signal == SIGSEGV, "expected SIGSEGV, got %d", got_signal);

	munmap(p, len);
	return 0;
}

static int test_mprotect_none_to_rw(void)
{
	char *p;
	size_t len = KB(64);

	TEST_LOG("mprotect NONE -> RW, verify access works");
	p = mmap(NULL, len, PROT_NONE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	int ret = mprotect(p, len, PROT_READ | PROT_WRITE);
	ASSERT(ret == 0, "mprotect: %s", strerror(errno));

	p[0] = 'X';
	ASSERT(p[0] == 'X', "write after NONE->RW");

	munmap(p, len);
	return 0;
}

static int test_mprotect_partial(void)
{
	char *p;
	size_t len = KB(128);

	TEST_LOG("mprotect on partial range (first 4K of 128K)");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	touch_range_write(p, len);

	/* Protect only the first 4K */
	int ret = mprotect(p, KB(4), PROT_READ);
	ASSERT(ret == 0, "mprotect: %s", strerror(errno));

	/* Writing past the protected range should still work */
	p[KB(4)] = 'Y';
	ASSERT(p[KB(4)] == 'Y', "write past protected range");

	/* Re-enable write on first 4K */
	ret = mprotect(p, KB(4), PROT_READ | PROT_WRITE);
	ASSERT(ret == 0, "mprotect re-enable: %s", strerror(errno));

	p[0] = 'Z';
	ASSERT(p[0] == 'Z', "write after re-enable");

	munmap(p, len);
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_mprotect_rw_to_ro();
	ret |= test_mprotect_none_to_rw();
	ret |= test_mprotect_partial();

	return ret ? 1 : 0;
}
