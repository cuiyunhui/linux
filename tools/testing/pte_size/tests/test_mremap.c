/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test mremap page faults.
 *
 * Exercises move_page_tables() and the PTE re-mapping paths.
 * With PTE_SIZE != PG_SIZE, mremap must correctly handle partial
 * folio remaps.
 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include "test_common.h"

static int test_mremap_grow(void)
{
	char *p, *q;
	size_t old_len = KB(64);
	size_t new_len = KB(256);

	TEST_LOG("mremap grow %zu -> %zu", old_len, new_len);
	p = mmap(NULL, old_len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	memset(p, 0xAA, old_len);

	q = mremap(p, old_len, new_len, MREMAP_MAYMOVE);
	ASSERT(q != MAP_FAILED, "mremap: %s", strerror(errno));

	/* Original data preserved */
	ASSERT(verify_range(q, old_len, (char)0xAA, 4096) == 0,
	       "data not preserved after mremap");

	/* New pages should fault in as zero */
	ASSERT(q[old_len] == 0, "new page not zeroed");

	/* Write to new region */
	memset(q + old_len, 0xBB, new_len - old_len);
	ASSERT((unsigned char)q[new_len - 1] == 0xBB, "new region write");

	munmap(q, new_len);
	return 0;
}

static int test_mremap_shrink(void)
{
	char *p, *q;
	size_t old_len = KB(256);
	size_t new_len = KB(64);

	TEST_LOG("mremap shrink %zu -> %zu", old_len, new_len);
	p = mmap(NULL, old_len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	memset(p, 0xCC, old_len);

	q = mremap(p, old_len, new_len, 0);
	ASSERT(q != MAP_FAILED, "mremap: %s", strerror(errno));

	/* Remaining data preserved */
	ASSERT(verify_range(q, new_len, (char)0xCC, 4096) == 0,
	       "data not preserved after shrink");

	munmap(q, new_len);
	return 0;
}

static int test_mremap_move(void)
{
	char *p, *q;
	size_t len = KB(64);

	TEST_LOG("mremap with forced move (MREMAP_MAYMOVE + MREMAP_FIXED)");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap src: %s", strerror(errno));

	/* Reserve a destination */
	char *dst = mmap(NULL, len, PROT_NONE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(dst != MAP_FAILED, "mmap dst: %s", strerror(errno));

	memset(p, 0xDD, len);

	q = mremap(p, len, len, MREMAP_MAYMOVE | MREMAP_FIXED,
		   (unsigned long)dst);
	ASSERT(q != MAP_FAILED, "mremap fixed: %s", strerror(errno));
	ASSERT(q == dst, "mremap didn't land at requested address");

	ASSERT(verify_range(q, len, (char)0xDD, 4096) == 0,
	       "data not preserved after move");

	munmap(q, len);
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_mremap_grow();
	ret |= test_mremap_shrink();
	ret |= test_mremap_move();

	return ret ? 1 : 0;
}
