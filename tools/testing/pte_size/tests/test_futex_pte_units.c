// SPDX-License-Identifier: GPL-2.0
/*
 * Verify that futex memory lookups use PTE-sized units when an allocator page
 * spans multiple PTEs. This mirrors a small pthread stack: the first PTE is a
 * guard and the child-tid futex lives in a later writable PTE.
 */
#include "test_common.h"
#include <linux/futex.h>
#include <sys/syscall.h>

#define PTE_BYTES	4096
#define TEST_PG_BYTES	(64 * 1024)
#define TEST_LOOPS	100

static void on_alarm(int sig)
{
	(void)sig;
	_exit(124);
}

static int futex_wait_mismatch(uint32_t *uaddr)
{
	long ret;

	errno = 0;
	ret = syscall(SYS_futex, uaddr,
		      FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, 0,
		      NULL, NULL, FUTEX_BITSET_MATCH_ANY);
	if (ret != -1)
		TEST_FAIL("futex wait unexpectedly returned %ld", ret);
	if (errno != EAGAIN)
		TEST_FAIL("futex wait returned %s, expected EAGAIN",
			  strerror(errno));
	return 0;
}

int main(void)
{
	unsigned char *map, *p;
	uint32_t *futex;
	uintptr_t aligned;
	int i;

	signal(SIGALRM, on_alarm);
	alarm(30);

	map = mmap(NULL, TEST_PG_BYTES * 2, PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED)
		TEST_FAIL("mmap: %s", strerror(errno));

	aligned = ((uintptr_t)map + TEST_PG_BYTES - 1) &
		  ~(uintptr_t)(TEST_PG_BYTES - 1);
	p = (unsigned char *)aligned;

	if (mprotect(p + PTE_BYTES, PTE_BYTES, PROT_READ | PROT_WRITE))
		TEST_FAIL("mprotect: %s", strerror(errno));

	/*
	 * Keep the futex away from the PTE boundary, like the child-tid word
	 * in a pthread descriptor near the top of a small stack mapping.
	 */
	futex = (uint32_t *)(p + PTE_BYTES + 512);
	*futex = 1;

	TEST_LOG("shared futex after a guard PTE");
	for (i = 0; i < TEST_LOOPS; i++) {
		if (futex_wait_mismatch(futex))
			return 1;
	}

	munmap(map, TEST_PG_BYTES * 2);
	return 0;
}
