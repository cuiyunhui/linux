/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test various mmap flag combinations and edge cases.
 *
 * Exercises different VMA configurations that stress PTE_SIZE != PG_SIZE
 * alignment and fault paths.
 */
#include "test_common.h"

static int test_map_fixed(void)
{
	char *base, *p;
	size_t len = KB(256);

	TEST_LOG("MAP_FIXED over existing mapping");
	base = mmap(NULL, len, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(base != MAP_FAILED, "mmap base: %s", strerror(errno));

	memset(base, 0xAA, len);

	/* Overwrite middle 64K with MAP_FIXED */
	p = mmap(base + KB(64), KB(64), PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT(p == base + KB(64), "MAP_FIXED didn't land at target");

	/* New mapping should be zeroed */
	ASSERT(p[0] == 0, "MAP_FIXED region not zeroed");

	/* Original regions should be untouched */
	ASSERT((unsigned char)base[0] == 0xAA, "pre-region corrupted");
	ASSERT((unsigned char)base[KB(128)] == 0xAA, "post-region corrupted");

	munmap(base, len);
	return 0;
}

static int test_map_populate(void)
{
	char *p;
	size_t len = KB(256);

	TEST_LOG("MAP_POPULATE (prefault all pages)");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Pages should already be faulted in */
	p[0] = 'A';
	p[len - 1] = 'Z';
	ASSERT(p[0] == 'A' && p[len - 1] == 'Z', "populate access");

	munmap(p, len);
	return 0;
}

static int test_map_noreserve(void)
{
	char *p;
	size_t len = MB(16);

	TEST_LOG("MAP_NORESERVE large mapping (%zuMB)", len / MB(1));
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Sparse access: touch only a few pages */
	for (size_t off = 0; off < len; off += MB(1)) {
		p[off] = 0x42;
		ASSERT(p[off] == 0x42, "sparse write at %zu", off);
	}

	munmap(p, len);
	return 0;
}

static int test_adjacent_mappings(void)
{
	char *p1, *p2;
	size_t len = KB(64);

	TEST_LOG("adjacent anonymous mappings");

	/* Map a large range, then split into two adjacent mappings */
	p1 = mmap(NULL, len * 2, PROT_READ | PROT_WRITE,
		  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p1 != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Unmap and remap in two halves at fixed addresses */
	p2 = p1 + len;
	munmap(p1, len * 2);

	p1 = mmap(p1, len, PROT_READ | PROT_WRITE,
		  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	p2 = mmap(p2, len, PROT_READ | PROT_WRITE,
		  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT(p1 != MAP_FAILED && p2 != MAP_FAILED, "mmap fixed pair");
	ASSERT(p2 == p1 + len, "mappings not adjacent");

	memset(p1, 0x11, len);
	memset(p2, 0x22, len);

	ASSERT((unsigned char)p1[len - 1] == 0x11, "first mapping end");
	ASSERT((unsigned char)p2[0] == 0x22, "second mapping start");

	munmap(p1, len);
	munmap(p2, len);
	return 0;
}

static int test_map_hugetlb_fallback(void)
{
	char *p;
	size_t len = MB(2);

	TEST_LOG("MAP_HUGETLB (expect ENOMEM fallback, non-fatal)");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (p == MAP_FAILED) {
		/* Expected if no huge pages configured */
		TEST_LOG("  hugetlb not available (expected in test VM)");
		return 0;
	}

	p[0] = 'H';
	ASSERT(p[0] == 'H', "hugetlb write");

	munmap(p, len);
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_map_fixed();
	ret |= test_map_populate();
	ret |= test_map_noreserve();
	ret |= test_adjacent_mappings();
	ret |= test_map_hugetlb_fallback();

	return ret ? 1 : 0;
}
