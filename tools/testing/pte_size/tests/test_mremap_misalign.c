/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test mremap to non-PG_SIZE-aligned address with anonymous pages.
 *
 * When PG_SIZE > PTE_SIZE, mremap(MREMAP_FIXED) can move an anonymous
 * mapping to a PTE-aligned but non-PG-aligned address.  This causes
 * vm_pteoff and vm_start to become misaligned: vm_pteoff still reflects
 * the original PG-aligned address, but vm_start is now non-PG-aligned.
 *
 * The page fault handler must handle this correctly — it cannot
 * ALIGN_DOWN(addr, PG_SIZE) blindly because that may land before
 * vm_start.
 */
#define _GNU_SOURCE
#include "test_common.h"
#include <sys/mman.h>

static int test_mremap_anon_misalign_basic(void)
{
	char *p, *q;
	size_t len = KB(64);

	TEST_LOG("mremap anon to non-PG-aligned addr, then verify");

	/* Allocate the source mapping (PG-aligned by kernel) */
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap src: %s", strerror(errno));

	/* Write a pattern */
	memset(p, 0xAA, len);

	/* Move to a fixed non-PG-aligned address */
	munmap((void *)0x40001000UL, len);
	q = mremap(p, len, len, MREMAP_MAYMOVE | MREMAP_FIXED,
		   0x40001000UL);
	ASSERT(q != MAP_FAILED, "mremap to misaligned: %s", strerror(errno));
	ASSERT(q == (char *)0x40001000UL, "mremap landed at wrong address");

	/* All data must be preserved across mremap */
	ASSERT(verify_range(q, len, (char)0xAA, KB(4)) == 0,
	       "data lost during mremap to misaligned addr");

	munmap(q, len);
	return 0;
}

static int test_mremap_anon_misalign_fault(void)
{
	char *p, *q, *dst;
	size_t len = KB(64);

	TEST_LOG("mremap anon misaligned, then fault in fresh pages");

	/* Allocate but DON'T touch — pages are not yet faulted in */
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Reserve misaligned destination */
	dst = mmap(NULL, len + KB(64), PROT_NONE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(dst != MAP_FAILED, "mmap dst: %s", strerror(errno));
	char *target = dst + KB(4);
	munmap(dst, len + KB(64));

	/* Move unfaulted mapping to misaligned address */
	q = mremap(p, len, len, MREMAP_MAYMOVE | MREMAP_FIXED,
		   (unsigned long)target);
	ASSERT(q != MAP_FAILED, "mremap: %s", strerror(errno));

	/*
	 * Now fault in pages at the misaligned address.
	 * do_anonymous_page() must handle vm_pteoff vs vm_start mismatch.
	 */
	touch_range_write(q, len);
	ASSERT(verify_range(q, len, 0x42, KB(4)) == 0, "fault verify");

	/* Read back to ensure no corruption */
	for (size_t off = 0; off < len; off += KB(4))
		ASSERT(q[off] == 0x42, "readback at %zu", off);

	munmap(q, len);
	return 0;
}

static int test_mremap_anon_misalign_cow(void)
{
	char *p, *q;
	size_t len = KB(64);
	pid_t pid;
	int status;
	/*
	 * Use a fixed, well-known target address to avoid address
	 * reuse issues from prior tests. 0x40000000 + 4K ensures
	 * non-PG-aligned destination.
	 */
	char *target = (char *)0x40001000UL;

	TEST_LOG("mremap anon misaligned + fork COW");

	/* Ensure target range is clear */
	munmap(target, len);

	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	memset(p, 0xCC, len);

	q = mremap(p, len, len, MREMAP_MAYMOVE | MREMAP_FIXED,
		   (unsigned long)target);
	ASSERT(q != MAP_FAILED, "mremap: %s", strerror(errno));

	/* Verify data before fork — check each 4K explicitly */
	for (size_t off = 0; off < len; off += KB(4))
		ASSERT((unsigned char)q[off] == 0xCC,
		       "pre-fork q[%zuK]=0x%02x (expected 0xCC)",
		       off / 1024, (unsigned char)q[off]);

	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));

	if (pid == 0) {
		/* Verify data in child before any writes */
		if ((unsigned char)q[KB(8)] != 0xCC)
			_exit(10);

		/* Child writes — COW at misaligned address */
		q[0] = 0xDD;
		q[KB(4)] = 0xEE;
		if (q[0] != (char)0xDD)
			_exit(1);
		if (q[KB(4)] != (char)0xEE)
			_exit(2);
		/* Untouched pages should still have parent data */
		if ((unsigned char)q[KB(8)] != 0xCC)
			_exit(3);
		_exit(0);
	}

	waitpid(pid, &status, 0);
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	       "child failed: %d", WEXITSTATUS(status));

	/* Parent unchanged */
	ASSERT((unsigned char)q[0] == 0xCC, "parent data");

	munmap(q, len);
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_mremap_anon_misalign_basic();
	ret |= test_mremap_anon_misalign_fault();
	ret |= test_mremap_anon_misalign_cow();

	return ret ? 1 : 0;
}
