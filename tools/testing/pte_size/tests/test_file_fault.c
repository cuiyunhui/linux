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

int main(void)
{
	int ret = 0;

	ret |= test_file_read_fault();
	ret |= test_file_cow_fault();
	ret |= test_file_shared_fault();
	ret |= test_file_offset_mmap();

	return ret ? 1 : 0;
}
