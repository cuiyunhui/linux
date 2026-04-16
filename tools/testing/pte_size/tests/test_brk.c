/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test brk page faults.
 *
 * Exercises the brk path which uses PTE_ALIGN boundaries on the
 * pte_size branch. Uses raw brk() syscall to avoid conflicts with
 * musl's internal heap management.
 */
#include "test_common.h"
#include <sys/syscall.h>

/*
 * Raw brk syscall: returns current/new break on success.
 * brk(0) returns current break. brk(addr) sets break to addr
 * and returns new break (which equals addr on success).
 */
static unsigned long raw_brk(unsigned long addr)
{
	return syscall(SYS_brk, addr);
}

static int test_brk_small(void)
{
	unsigned long cur, new_brk;
	char *p;

	TEST_LOG("brk: grow by 4K and write");
	cur = raw_brk(0);
	ASSERT(cur != 0, "brk(0) returned 0");

	new_brk = raw_brk(cur + KB(4));
	ASSERT(new_brk == cur + KB(4),
	       "brk grow failed: got 0x%lx, expected 0x%lx",
	       new_brk, cur + KB(4));

	p = (char *)cur;
	memset(p, 0x55, KB(4));
	ASSERT((unsigned char)p[0] == 0x55, "first byte");
	ASSERT((unsigned char)p[KB(4) - 1] == 0x55, "last byte");

	return 0;
}

static int test_brk_large(void)
{
	unsigned long start, cur;
	size_t target = MB(1);

	TEST_LOG("brk: incremental growth to 1MB");
	start = raw_brk(0);
	ASSERT(start != 0, "brk(0)");

	/* Grow in mixed increments */
	size_t increments[] = { KB(4), KB(16), KB(64), KB(4), KB(128) };
	int n = sizeof(increments) / sizeof(increments[0]);
	size_t total = 0;
	cur = start;
	int step = 0;

	while (total < target) {
		size_t inc = increments[step % n];
		if (total + inc > target)
			inc = target - total;

		unsigned long new_brk = raw_brk(cur + inc);
		ASSERT(new_brk == cur + inc,
		       "brk step %d: got 0x%lx, expected 0x%lx",
		       step, new_brk, cur + inc);

		memset((char *)cur, 0x66, inc);
		cur = new_brk;
		total += inc;
		step++;
	}

	ASSERT(total == target, "grew %zu, expected %zu", total, target);
	return 0;
}

static int test_brk_shrink(void)
{
	unsigned long base, grown;
	size_t len = KB(256);

	TEST_LOG("brk: grow then shrink then regrow");
	base = raw_brk(0);
	ASSERT(base != 0, "brk(0)");

	grown = raw_brk(base + len);
	ASSERT(grown == base + len, "brk grow");

	memset((char *)base, 0x77, len);

	/* Shrink back */
	unsigned long shrunk = raw_brk(base);
	ASSERT(shrunk == base, "brk shrink: got 0x%lx, expected 0x%lx",
	       shrunk, base);

	/* Grow again — should get zeroed memory */
	unsigned long regrown = raw_brk(base + KB(4));
	ASSERT(regrown == base + KB(4), "brk regrow");

	/* New pages must be zeroed (kernel guarantees this) */
	char *p = (char *)base;
	ASSERT(p[0] == 0, "re-grown memory not zeroed: got 0x%02x",
	       (unsigned char)p[0]);

	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_brk_small();
	ret |= test_brk_large();
	ret |= test_brk_shrink();

	return ret ? 1 : 0;
}
