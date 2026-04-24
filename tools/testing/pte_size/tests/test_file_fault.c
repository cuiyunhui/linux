/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test file-backed page faults.
 *
 * Creates temp files, mmaps them, and exercises read/write faults.
 * Tests do_read_fault, do_cow_fault, do_shared_fault paths.
 */
#include "test_common.h"

static int test_file_read_fault(void)
{
	int fd;
	char *p;
	size_t len = KB(64);
	char buf[4096];

	TEST_LOG("file-backed read fault (MAP_PRIVATE)");

	fd = open("/tmp/test_read", O_CREAT | O_RDWR | O_TRUNC, 0644);
	ASSERT(fd >= 0, "open: %s", strerror(errno));

	/* Write a known pattern */
	memset(buf, 0xAA, sizeof(buf));
	for (size_t off = 0; off < len; off += sizeof(buf))
		write(fd, buf, sizeof(buf));

	p = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Read every 4K — each triggers a read fault */
	ASSERT(verify_range(p, len, (char)0xAA, 4096) == 0,
	       "file data mismatch");

	munmap(p, len);
	close(fd);
	unlink("/tmp/test_read");
	return 0;
}

static int test_file_cow_fault(void)
{
	int fd;
	char *p;
	size_t len = KB(64);
	char buf[4096];

	TEST_LOG("file-backed COW fault (MAP_PRIVATE + write)");

	fd = open("/tmp/test_cow", O_CREAT | O_RDWR | O_TRUNC, 0644);
	ASSERT(fd >= 0, "open: %s", strerror(errno));

	memset(buf, 0xBB, sizeof(buf));
	for (size_t off = 0; off < len; off += sizeof(buf))
		write(fd, buf, sizeof(buf));

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Read first (read fault), then write (COW fault) */
	ASSERT(verify_range(p, len, (char)0xBB, 4096) == 0,
	       "initial read mismatch");

	touch_range_write(p, len);
	ASSERT(verify_range(p, len, 0x42, 4096) == 0,
	       "post-COW data mismatch");

	/* Verify file is unchanged */
	lseek(fd, 0, SEEK_SET);
	read(fd, buf, sizeof(buf));
	ASSERT((unsigned char)buf[0] == 0xBB, "file data changed after COW");

	munmap(p, len);
	close(fd);
	unlink("/tmp/test_cow");
	return 0;
}

static int test_file_shared_fault(void)
{
	int fd;
	char *p;
	size_t len = KB(64);
	char buf[4096];

	TEST_LOG("file-backed shared write fault (MAP_SHARED)");

	fd = open("/tmp/test_shared", O_CREAT | O_RDWR | O_TRUNC, 0644);
	ASSERT(fd >= 0, "open: %s", strerror(errno));

	memset(buf, 0, sizeof(buf));
	for (size_t off = 0; off < len; off += sizeof(buf))
		write(fd, buf, sizeof(buf));

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Write through shared mapping */
	memset(p, 0xDD, len);
	msync(p, len, MS_SYNC);

	/* Read back via file to verify writeback */
	lseek(fd, 0, SEEK_SET);
	read(fd, buf, sizeof(buf));
	ASSERT((unsigned char)buf[0] == 0xDD,
	       "shared write not visible in file");

	munmap(p, len);
	close(fd);
	unlink("/tmp/test_shared");
	return 0;
}

static int test_file_offset_mmap(void)
{
	int fd;
	char *p;
	size_t file_len = KB(256);
	size_t map_len = KB(64);
	off_t offset = KB(64);
	char buf[4096];

	TEST_LOG("file mmap at non-zero offset");

	fd = open("/tmp/test_offset", O_CREAT | O_RDWR | O_TRUNC, 0644);
	ASSERT(fd >= 0, "open: %s", strerror(errno));

	/* Write different patterns at different offsets */
	memset(buf, 0x11, sizeof(buf));
	for (size_t off = 0; off < file_len; off += sizeof(buf)) {
		buf[0] = (off / KB(64)) + 1; /* Different per 64K chunk */
		lseek(fd, off, SEEK_SET);
		write(fd, buf, sizeof(buf));
	}

	/* Map the second 64K chunk */
	p = mmap(NULL, map_len, PROT_READ, MAP_PRIVATE, fd, offset);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* First byte of mapped region should match file[offset] */
	ASSERT(p[0] == 2, "offset mmap: expected 2, got %d", p[0]);

	munmap(p, map_len);
	close(fd);
	unlink("/tmp/test_offset");
	return 0;
}

/*
 * Write-only pattern: MAP_PRIVATE a file with write permission, then
 * write to every PTE-sized sub-region of every PG without reading
 * anywhere first. With no prior read-fault, every write goes through
 * do_cow_fault() against a pte_none PTE (as opposed to the wp_page_copy()
 * path used when a file PTE already exists). The sibling-PTE install
 * inside finish_fault() must map all PTEs of the same PG to the new
 * COW folio in one shot; otherwise subsequent sub-PTE writes COW into
 * unrelated folios and lose each other's data. This mirrors ld-linux's
 * relocation pattern across libperl.so .data.
 */
static int test_file_cow_fault_no_preread(void)
{
	int fd;
	uint64_t *p;
	size_t len = KB(64);
	uint64_t file_pattern = 0x1234567890abcdefULL;
	size_t i;

	TEST_LOG("write-only scattered COW (no read-prime)");
	fd = open("/tmp/test_nopreread", O_CREAT | O_RDWR | O_TRUNC, 0644);
	ASSERT(fd >= 0, "open: %s", strerror(errno));

	for (i = 0; i < len / sizeof(file_pattern); i++)
		ASSERT(write(fd, &file_pattern, sizeof(file_pattern)) ==
			       sizeof(file_pattern),
		       "write: %s", strerror(errno));

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Skip read-prime: first touch per PG is a write fault, which
	 * exercises do_cow_fault() + the sibling install path. Write
	 * specific offsets within every PG. */
	for (i = 0; i < len / sizeof(*p); i++) {
		size_t byte_off = i * sizeof(*p);
		size_t chunk_off = byte_off & 0xfff;
		if (chunk_off == 0x0 || chunk_off == 0x8 ||
		    chunk_off == 0x10 || chunk_off == 0x18 ||
		    chunk_off == 0x20 || chunk_off == 0x28 ||
		    chunk_off == 0x40 || chunk_off == 0x48)
			p[i] = (uint64_t)0xBBBB000000000000ULL | (byte_off & 0xffffffff);
	}

	for (i = 0; i < len / sizeof(*p); i++) {
		size_t byte_off = i * sizeof(*p);
		size_t chunk_off = byte_off & 0xfff;
		uint64_t expected;

		if (chunk_off == 0x0 || chunk_off == 0x8 ||
		    chunk_off == 0x10 || chunk_off == 0x18 ||
		    chunk_off == 0x20 || chunk_off == 0x28 ||
		    chunk_off == 0x40 || chunk_off == 0x48)
			expected = (uint64_t)0xBBBB000000000000ULL | (byte_off & 0xffffffff);
		else
			expected = file_pattern;

		ASSERT(p[i] == expected,
		       "post-write mismatch at byte_off %zx: got %lx, want %lx",
		       byte_off, p[i], expected);
	}

	munmap(p, len);
	close(fd);
	unlink("/tmp/test_nopreread");
	return 0;
}

/*
 * Mimic ld-linux.so's relocation-write pattern: MAP_PRIVATE a file,
 * read it first (to fault in file PTEs), then write small scattered
 * qwords to different offsets across the PG_SIZE boundary, and verify
 * that every write survives.
 */
static int test_file_multi_write_per_pg(void)
{
	int fd;
	uint64_t *p;
	size_t len = KB(64);
	uint64_t file_pattern = 0x1234567890abcdefULL;
	size_t i;

	TEST_LOG("scattered qword writes in a single PG_SIZE-backed PG");
	fd = open("/tmp/test_multi", O_CREAT | O_RDWR | O_TRUNC, 0644);
	ASSERT(fd >= 0, "open: %s", strerror(errno));

	for (i = 0; i < len / sizeof(file_pattern); i++)
		ASSERT(write(fd, &file_pattern, sizeof(file_pattern)) ==
			       sizeof(file_pattern),
		       "write: %s", strerror(errno));

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Read-prime: fault in file PTEs for the whole region. */
	for (i = 0; i < len / sizeof(*p); i++)
		ASSERT(p[i] == file_pattern,
		       "initial read mismatch at qword %zu: got %lx",
		       i, p[i]);

	/* Write at *every* qword offset in a specific pattern that
	 * covers offsets 0x0, 0x8, 0x10, 0x18, 0x20, 0x28, 0x40, 0x48
	 * within every 4K chunk — matching ld-linux's dense relocation
	 * write pattern on libperl.so .data. */
	for (i = 0; i < len / sizeof(*p); i++) {
		size_t byte_off = i * sizeof(*p);
		size_t chunk_off = byte_off & 0xfff;
		/* Write at specific offsets mimicking R_X86_64_RELATIVE +
		 * R_X86_64_64 targets seen in libperl.so. */
		if (chunk_off == 0x0 || chunk_off == 0x8 ||
		    chunk_off == 0x10 || chunk_off == 0x18 ||
		    chunk_off == 0x20 || chunk_off == 0x28 ||
		    chunk_off == 0x40 || chunk_off == 0x48)
			p[i] = (uint64_t)0xAAAA000000000000ULL | (byte_off & 0xffffffff);
	}

	/* Verify every write persisted. */
	for (i = 0; i < len / sizeof(*p); i++) {
		size_t byte_off = i * sizeof(*p);
		size_t chunk_off = byte_off & 0xfff;
		uint64_t expected;

		if (chunk_off == 0x0 || chunk_off == 0x8 ||
		    chunk_off == 0x10 || chunk_off == 0x18 ||
		    chunk_off == 0x20 || chunk_off == 0x28 ||
		    chunk_off == 0x40 || chunk_off == 0x48)
			expected = (uint64_t)0xAAAA000000000000ULL | (byte_off & 0xffffffff);
		else
			expected = file_pattern;

		ASSERT(p[i] == expected,
		       "post-write mismatch at byte_off %zx (chunk_off %zx): got %lx, want %lx",
		       byte_off, chunk_off, p[i], expected);
	}

	munmap(p, len);
	close(fd);
	unlink("/tmp/test_multi");
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_file_read_fault();
	ret |= test_file_cow_fault();
	ret |= test_file_shared_fault();
	ret |= test_file_offset_mmap();
	ret |= test_file_cow_fault_no_preread();
	ret |= test_file_multi_write_per_pg();

	return ret ? 1 : 0;
}
