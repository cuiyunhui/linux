/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test file-backed mappings with non-PG_SIZE-aligned offsets near
 * page table boundaries.
 *
 * When PG_SIZE > PTE_SIZE, a file mapping with offset not a multiple
 * of PG_SIZE creates a vm_pteoff that is misaligned within the
 * PG_SIZE group.  This can cause the folio's PTE group to straddle
 * a page table (PMD) boundary if the VMA is placed near one.
 *
 * On x86-64 with PTE_SIZE=4K, the PMD boundary is at every 2MB.
 */
#include "test_common.h"

#define PMD_SIZE_GUESS	MB(2)

static int create_test_file(const char *path, size_t size)
{
	int fd;
	char buf[4096];

	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0)
		return -1;

	memset(buf, 0xAA, sizeof(buf));
	for (size_t off = 0; off < size; off += sizeof(buf))
		write(fd, buf, sizeof(buf));

	return fd;
}

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
 * Test: file mapping with 4K offset (non-PG-aligned).
 * With PG_SIZE=16K, offset=4K means vm_pteoff%4=1.
 */
static int test_file_offset_4k(void)
{
	int fd;
	char *p;
	size_t file_size = KB(256);
	size_t map_len = KB(64);
	off_t offset = KB(4);

	TEST_LOG("file mmap with 4K offset (non-PG-aligned)");

	fd = create_test_file("/tmp/test_foff", file_size);
	ASSERT(fd >= 0, "create file: %s", strerror(errno));

	p = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE, fd, offset);
	ASSERT(p != MAP_FAILED, "mmap offset=4K: %s", strerror(errno));

	/* Read fault at misaligned offset */
	ASSERT((unsigned char)p[0] == 0xAA, "read at offset 4K");
	ASSERT((unsigned char)p[KB(4)] == 0xAA, "read at offset 8K");

	/* COW fault at misaligned offset */
	p[0] = 0xBB;
	ASSERT(p[0] == (char)0xBB, "COW at offset 4K");
	p[KB(4)] = 0xCC;
	ASSERT(p[KB(4)] == (char)0xCC, "COW at offset 8K");

	/* Verify file is unchanged */
	{
		char buf[1];
		lseek(fd, offset, SEEK_SET);
		read(fd, buf, 1);
		ASSERT((unsigned char)buf[0] == 0xAA, "file unchanged");
	}

	munmap(p, map_len);
	close(fd);
	unlink("/tmp/test_foff");
	return 0;
}

/*
 * Test: file mapping with 4K offset placed at a PMD boundary.
 * This creates the worst case: misaligned vm_pteoff AND the
 * folio's PTE group straddles two page tables.
 */
static int test_file_offset_at_pmd_boundary(void)
{
	int fd;
	char *region, *base, *p;
	size_t region_size = PMD_SIZE_GUESS * 4;
	size_t file_size = KB(256);
	size_t map_len = KB(32);
	off_t offset = KB(4);
	unsigned long boundary;

	TEST_LOG("file mmap offset=4K at PMD boundary");

	fd = create_test_file("/tmp/test_fpmd", file_size);
	ASSERT(fd >= 0, "create file: %s", strerror(errno));

	region = find_free_region(region_size);
	ASSERT(region != NULL, "find_free_region");

	/* Place mapping so it straddles a PMD boundary */
	boundary = ((unsigned long)region + PMD_SIZE_GUESS) &
		   ~(PMD_SIZE_GUESS - 1);
	base = (char *)(boundary - KB(16));

	p = mmap(base, map_len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_FIXED, fd, offset);
	ASSERT(p == base, "mmap MAP_FIXED: %s", strerror(errno));

	/*
	 * Read faults across the boundary.  With offset=4K and
	 * PG_SIZE=16K, the folio's first PTE is at sub-page offset 1.
	 * Near the boundary, some PTEs are in one page table and
	 * some in the next.
	 */
	for (size_t off = 0; off < map_len; off += KB(4)) {
		volatile char c = p[off];
		(void)c;
	}
	ASSERT((unsigned char)p[0] == 0xAA, "read before boundary");
	ASSERT((unsigned char)p[KB(16)] == 0xAA, "read after boundary");

	/* COW faults on both sides */
	p[KB(12)] = 0x11;  /* just before PMD boundary */
	p[KB(16)] = 0x22;  /* right at/after PMD boundary */
	ASSERT(p[KB(12)] == 0x11, "COW before boundary");
	ASSERT(p[KB(16)] == 0x22, "COW after boundary");

	munmap(p, map_len);
	close(fd);
	unlink("/tmp/test_fpmd");
	return 0;
}

/*
 * Test: file mapping with various non-aligned offsets.
 */
static int test_file_various_offsets(void)
{
	int fd;
	size_t file_size = KB(256);
	off_t offsets[] = { KB(4), KB(8), KB(12) };
	int n = sizeof(offsets) / sizeof(offsets[0]);

	TEST_LOG("file mmap with various non-PG-aligned offsets");

	fd = create_test_file("/tmp/test_fvar", file_size);
	ASSERT(fd >= 0, "create file: %s", strerror(errno));

	for (int i = 0; i < n; i++) {
		char *p;
		size_t map_len = KB(32);

		p = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE, fd, offsets[i]);
		ASSERT(p != MAP_FAILED, "mmap offset=%ldK: %s",
		       offsets[i] / 1024, strerror(errno));

		/* Read all pages */
		for (size_t off = 0; off < map_len; off += KB(4))
			ASSERT((unsigned char)p[off] == 0xAA,
			       "read at file offset %ldK + %zuK",
			       offsets[i] / 1024, off / 1024);

		/* COW all pages */
		for (size_t off = 0; off < map_len; off += KB(4))
			p[off] = 0x42;
		ASSERT(verify_range(p, map_len, 0x42, KB(4)) == 0,
		       "COW verify offset=%ldK", offsets[i] / 1024);

		munmap(p, map_len);
	}

	close(fd);
	unlink("/tmp/test_fvar");
	return 0;
}

/*
 * Test: COW with fork on file mapping with non-aligned offset.
 */
static int test_file_offset_cow_fork(void)
{
	int fd;
	char *p;
	size_t file_size = KB(256);
	size_t map_len = KB(64);
	off_t offset = KB(4);
	pid_t pid;
	int status;

	TEST_LOG("file mmap offset=4K + fork COW");

	fd = create_test_file("/tmp/test_fcow", file_size);
	ASSERT(fd >= 0, "create file: %s", strerror(errno));

	p = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE, fd, offset);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Parent reads to fault in file pages */
	for (size_t off = 0; off < map_len; off += KB(4))
		ASSERT((unsigned char)p[off] == 0xAA,
		       "initial read at %zuK", off / 1024);

	/* Parent writes some pages (COW) */
	p[0] = 0x11;
	p[KB(4)] = 0x22;

	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));

	if (pid == 0) {
		/* Child: verify parent's writes visible */
		if (p[0] != 0x11)
			_exit(1);
		if (p[KB(4)] != 0x22)
			_exit(2);

		/* Child writes (COW from parent's COW page) */
		p[0] = 0x33;
		p[KB(8)] = 0x44;

		if (p[0] != 0x33)
			_exit(3);
		if (p[KB(8)] != 0x44)
			_exit(4);

		/* Verify non-written pages still have file data */
		if ((unsigned char)p[KB(16)] != 0xAA)
			_exit(5);
		_exit(0);
	}

	waitpid(pid, &status, 0);
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	       "child failed: exit=%d", WEXITSTATUS(status));

	/* Parent data unchanged */
	ASSERT(p[0] == 0x11, "parent p[0] after child COW");
	ASSERT(p[KB(4)] == 0x22, "parent p[4K] after child COW");

	munmap(p, map_len);
	close(fd);
	unlink("/tmp/test_fcow");
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_file_offset_4k();
	ret |= test_file_offset_at_pmd_boundary();
	ret |= test_file_various_offsets();
	ret |= test_file_offset_cow_fork();

	return ret ? 1 : 0;
}
