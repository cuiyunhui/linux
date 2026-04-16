/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test page faults near PMD (page table) boundaries.
 *
 * With PG_SIZE > PTE_SIZE, a single folio maps to multiple PTEs.
 * When a VMA crosses a PMD boundary, the folio's PTEs may need to
 * be split across two page tables.  Verify the kernel handles this
 * by clamping PTE installation to page table boundaries.
 *
 * On x86 with 4K PTE_SIZE, PMD boundary is every 2MB (512 PTEs).
 * We use MAP_FIXED to place mappings precisely at these boundaries.
 */
#include "test_common.h"

/* PMD_SIZE on x86-64 with 4K PTE_SIZE = 2MB */
#define PMD_SIZE_GUESS	MB(2)

/*
 * Find a usable base address for MAP_FIXED tests.
 * Map a large region, find its address, unmap, and use that area.
 */
static char *find_free_region(size_t size)
{
	char *p = mmap(NULL, size, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	munmap(p, size);
	return p;
}

/*
 * Test: anonymous mapping that straddles a PMD boundary.
 * Map [boundary - offset, boundary + remaining) and write to both sides.
 */
static int test_anon_straddle_pmd(void)
{
	char *region, *base, *p;
	size_t region_size = PMD_SIZE_GUESS * 4;
	size_t before = KB(4);   /* 4K before boundary */
	size_t after = KB(12);   /* 12K after boundary */
	size_t total = before + after;
	unsigned long boundary;

	TEST_LOG("anonymous mapping straddling PMD boundary");

	region = find_free_region(region_size);
	ASSERT(region != NULL, "find_free_region");

	/* Align up to next PMD boundary, then back off */
	boundary = ((unsigned long)region + PMD_SIZE_GUESS) &
		   ~(PMD_SIZE_GUESS - 1);
	base = (char *)(boundary - before);

	p = mmap(base, total, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT(p == base, "mmap MAP_FIXED at PMD boundary: %s",
	       strerror(errno));

	/* Write before the boundary (last PTE in one page table) */
	p[0] = 0x11;
	ASSERT(p[0] == 0x11, "write before boundary");

	/* Write after the boundary (first PTE in next page table) */
	p[before] = 0x22;
	ASSERT(p[before] == 0x22, "write after boundary");

	/* Write to every 4K offset across the boundary */
	touch_range_write(p, total);
	ASSERT(verify_range(p, total, 0x42, KB(4)) == 0, "data verify");

	munmap(p, total);
	return 0;
}

/*
 * Test: COW fault straddling a PMD boundary.
 * Fork, then write to pages on both sides of the boundary.
 */
static int test_cow_straddle_pmd(void)
{
	char *region, *base, *p;
	size_t region_size = PMD_SIZE_GUESS * 4;
	size_t before = KB(8);
	size_t after = KB(8);
	size_t total = before + after;
	unsigned long boundary;
	pid_t pid;
	int status;

	TEST_LOG("COW fault straddling PMD boundary");

	region = find_free_region(region_size);
	ASSERT(region != NULL, "find_free_region");

	boundary = ((unsigned long)region + PMD_SIZE_GUESS) &
		   ~(PMD_SIZE_GUESS - 1);
	base = (char *)(boundary - before);

	p = mmap(base, total, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT(p == base, "mmap: %s", strerror(errno));

	/* Parent writes pattern */
	memset(p, 0xAA, total);

	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));

	if (pid == 0) {
		/* Child: write across the boundary to trigger COW */
		p[before - KB(4)] = 0xBB; /* last 4K before boundary */
		p[before] = 0xCC;         /* first 4K after boundary */

		if (p[before - KB(4)] != (char)0xBB)
			_exit(1);
		if (p[before] != (char)0xCC)
			_exit(2);
		/* Verify parent's data in untouched pages */
		if (p[0] != (char)0xAA)
			_exit(3);
		if (p[total - KB(4)] != (char)0xAA)
			_exit(4);
		_exit(0);
	}

	waitpid(pid, &status, 0);
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	       "child failed: exit=%d", WEXITSTATUS(status));

	/* Parent data should be unchanged */
	ASSERT((unsigned char)p[before - KB(4)] == 0xAA,
	       "parent data before boundary corrupted");
	ASSERT((unsigned char)p[before] == 0xAA,
	       "parent data after boundary corrupted");

	munmap(p, total);
	return 0;
}

/*
 * Test: file-backed COW straddling PMD boundary.
 * Create a file, mmap it MAP_PRIVATE crossing the boundary, write.
 */
static int test_file_cow_straddle_pmd(void)
{
	char *region, *base, *p;
	int fd;
	char buf[4096];
	size_t region_size = PMD_SIZE_GUESS * 4;
	size_t before = KB(8);
	size_t after = KB(8);
	size_t total = before + after;
	unsigned long boundary;

	TEST_LOG("file-backed COW straddling PMD boundary");

	fd = open("/tmp/test_pmd", O_CREAT | O_RDWR | O_TRUNC, 0644);
	ASSERT(fd >= 0, "open: %s", strerror(errno));

	/* Write enough data for the mapping */
	memset(buf, 0xDD, sizeof(buf));
	for (size_t off = 0; off < total; off += sizeof(buf))
		write(fd, buf, sizeof(buf));

	region = find_free_region(region_size);
	ASSERT(region != NULL, "find_free_region");

	boundary = ((unsigned long)region + PMD_SIZE_GUESS) &
		   ~(PMD_SIZE_GUESS - 1);
	base = (char *)(boundary - before);

	p = mmap(base, total, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_FIXED, fd, 0);
	ASSERT(p == base, "mmap: %s", strerror(errno));

	/* Read across boundary (file read faults) */
	ASSERT((unsigned char)p[0] == 0xDD, "read before boundary");
	ASSERT((unsigned char)p[before] == 0xDD, "read after boundary");

	/* Write across boundary (COW faults) */
	p[before - KB(4)] = 0xEE;
	p[before] = 0xFF;
	ASSERT((unsigned char)p[before - KB(4)] == 0xEE,
	       "COW before boundary");
	ASSERT((unsigned char)p[before] == 0xFF,
	       "COW after boundary");

	/* Verify file is unchanged */
	lseek(fd, 0, SEEK_SET);
	read(fd, buf, sizeof(buf));
	ASSERT((unsigned char)buf[0] == 0xDD, "file unchanged");

	munmap(p, total);
	close(fd);
	unlink("/tmp/test_pmd");
	return 0;
}

/*
 * Test: munmap that splits a mapping at a PMD boundary.
 * Map a region crossing the boundary, unmap the middle.
 */
static int test_munmap_at_pmd(void)
{
	char *region, *base, *p;
	size_t region_size = PMD_SIZE_GUESS * 4;
	size_t total = KB(64);
	unsigned long boundary;

	TEST_LOG("munmap at PMD boundary");

	region = find_free_region(region_size);
	ASSERT(region != NULL, "find_free_region");

	boundary = ((unsigned long)region + PMD_SIZE_GUESS) &
		   ~(PMD_SIZE_GUESS - 1);
	/* Center 64K around the boundary */
	base = (char *)(boundary - KB(32));

	p = mmap(base, total, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT(p == base, "mmap: %s", strerror(errno));

	memset(p, 0xAA, total);

	/* Unmap 4K right at the boundary */
	int ret = munmap((char *)boundary, KB(4));
	ASSERT(ret == 0, "munmap at boundary: %s", strerror(errno));

	/* Both sides should still be accessible */
	ASSERT((unsigned char)p[0] == 0xAA, "before boundary");
	ASSERT((unsigned char)((char *)boundary)[KB(4)] == 0xAA,
	       "after boundary + 4K");

	munmap(p, boundary - (unsigned long)p);
	munmap((char *)boundary + KB(4),
	       total - (boundary - (unsigned long)p) - KB(4));
	return 0;
}

/*
 * Test: mprotect across a PMD boundary.
 */
static int test_mprotect_at_pmd(void)
{
	char *region, *base, *p;
	size_t region_size = PMD_SIZE_GUESS * 4;
	size_t total = KB(64);
	unsigned long boundary;

	TEST_LOG("mprotect across PMD boundary");

	region = find_free_region(region_size);
	ASSERT(region != NULL, "find_free_region");

	boundary = ((unsigned long)region + PMD_SIZE_GUESS) &
		   ~(PMD_SIZE_GUESS - 1);
	base = (char *)(boundary - KB(32));

	p = mmap(base, total, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT(p == base, "mmap: %s", strerror(errno));

	touch_range_write(p, total);

	/* mprotect the first half (up to boundary) to read-only */
	int ret = mprotect(p, boundary - (unsigned long)p, PROT_READ);
	ASSERT(ret == 0, "mprotect: %s", strerror(errno));

	/* Read should work on both sides */
	ASSERT((unsigned char)p[0] == 0x42, "read RO region");
	ASSERT((unsigned char)((char *)boundary)[0] == 0x42,
	       "read RW region");

	/* Write after boundary should still work */
	((char *)boundary)[0] = 0x55;
	ASSERT(((char *)boundary)[0] == 0x55, "write after boundary");

	/* Restore permissions */
	mprotect(p, boundary - (unsigned long)p, PROT_READ | PROT_WRITE);
	munmap(p, total);
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_anon_straddle_pmd();
	ret |= test_cow_straddle_pmd();
	ret |= test_file_cow_straddle_pmd();
	ret |= test_munmap_at_pmd();
	ret |= test_mprotect_at_pmd();

	return ret ? 1 : 0;
}
