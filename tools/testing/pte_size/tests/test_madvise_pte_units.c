// SPDX-License-Identifier: GPL-2.0
/*
 * Exercise PTE-sized madvise ranges when one allocator page spans multiple
 * PTEs. In particular, every operation must make forward progress when the
 * advised range covers only part of a multi-PTE folio.
 */
#include "test_common.h"
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>

#define PTE_BYTES	4096
#define TEST_PG_BYTES	(64 * 1024)
#define FILE_BYTES	(4 * 1024 * 1024)
#define TEST_LOOPS	100

static void on_alarm(int sig)
{
	(void)sig;
	_exit(124);
}

static int advise_range(void *addr, size_t len, int advice,
			const char *name)
{
	if (madvise(addr, len, advice))
		TEST_FAIL("madvise(%s, offset=%zu, len=%zu): %s", name,
			  (size_t)((uintptr_t)addr & (TEST_PG_BYTES - 1)),
			  len, strerror(errno));
	return 0;
}

static int exercise_anon_ranges(void)
{
	unsigned char *map, *p;
	uintptr_t aligned;
	size_t off;
	int i;

	map = mmap(NULL, TEST_PG_BYTES * 2, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED)
		TEST_FAIL("anonymous mmap: %s", strerror(errno));

	aligned = ((uintptr_t)map + TEST_PG_BYTES - 1) &
		  ~(uintptr_t)(TEST_PG_BYTES - 1);
	p = (unsigned char *)aligned;

	for (i = 0; i < TEST_LOOPS; i++) {
		for (off = 0; off < TEST_PG_BYTES; off += PTE_BYTES)
			p[off] = (unsigned char)(i + off / PTE_BYTES);

		if (advise_range(p + PTE_BYTES, PTE_BYTES, MADV_COLD,
				 "COLD"))
			return 1;
		if (advise_range(p + PTE_BYTES,
				 TEST_PG_BYTES - PTE_BYTES, MADV_PAGEOUT,
				 "PAGEOUT"))
			return 1;
		if (advise_range(p, TEST_PG_BYTES, MADV_FREE, "FREE"))
			return 1;
	}

	munmap(map, TEST_PG_BYTES * 2);
	return 0;
}

static int fill_file(int fd)
{
	unsigned char buf[PTE_BYTES];
	size_t off;
	ssize_t written;

	for (off = 0; off < FILE_BYTES; off += sizeof(buf)) {
		memset(buf, (unsigned char)(off / sizeof(buf)), sizeof(buf));
		written = pwrite(fd, buf, sizeof(buf), off);
		if (written != (ssize_t)sizeof(buf))
			TEST_FAIL("pwrite at %zu: %s", off, strerror(errno));
	}
	return 0;
}

static int exercise_file_ranges(void)
{
	char path[] = "/tmp/madvise-pte-XXXXXX";
	unsigned char *map;
	unsigned char expected;
	size_t off;
	int fd, i, ret;

	fd = mkstemp(path);
	if (fd < 0)
		TEST_FAIL("mkstemp: %s", strerror(errno));
	unlink(path);

	if (fill_file(fd))
		return 1;
	ret = posix_fadvise(fd, 0, FILE_BYTES, POSIX_FADV_SEQUENTIAL);
	if (ret)
		TEST_FAIL("posix_fadvise(SEQUENTIAL): %s", strerror(ret));
	ret = posix_fadvise(fd, 0, FILE_BYTES, POSIX_FADV_WILLNEED);
	if (ret)
		TEST_FAIL("posix_fadvise(WILLNEED): %s", strerror(ret));

	map = mmap(NULL, FILE_BYTES, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED)
		TEST_FAIL("file mmap: %s", strerror(errno));

	for (off = 0; off < FILE_BYTES; off += PTE_BYTES) {
		expected = (unsigned char)(off / PTE_BYTES);
		if (map[off] != expected)
			TEST_FAIL("initial file mismatch at %zu", off);
	}

	for (i = 0; i < TEST_LOOPS; i++) {
		off = ((size_t)i * TEST_PG_BYTES) % (FILE_BYTES - TEST_PG_BYTES);
		if (advise_range(map + off + PTE_BYTES, PTE_BYTES,
				 MADV_COLD, "file COLD"))
			return 1;
		if (advise_range(map + off + PTE_BYTES,
				 TEST_PG_BYTES - PTE_BYTES, MADV_PAGEOUT,
				 "file PAGEOUT"))
			return 1;
		expected = (unsigned char)(off / PTE_BYTES + 1);
		if (map[off + PTE_BYTES] != expected)
			TEST_FAIL("file mismatch after PAGEOUT at %zu",
				  off + PTE_BYTES);
	}

	munmap(map, FILE_BYTES);
	close(fd);
	return 0;
}

int main(void)
{
	signal(SIGALRM, on_alarm);
	alarm(30);

	TEST_LOG("anonymous sub-PG MADV_COLD/PAGEOUT/FREE");
	if (exercise_anon_ranges())
		return 1;

	TEST_LOG("file-backed sub-PG MADV_COLD/PAGEOUT after readahead");
	if (exercise_file_ranges())
		return 1;

	return 0;
}
