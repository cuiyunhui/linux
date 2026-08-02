// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PTE_SIZE_BYTES	4096UL
#define FILE_PTES	64UL
#define REMAP_FIRST	1UL
#define REMAP_PTES	17UL
#define REMAP_PGOFF	32UL

static uint64_t pattern(unsigned long pte)
{
	return UINT64_C(0x9e3779b97f4a7c15) ^
	       (UINT64_C(0x100000001b3) * (pte + 1));
}

static int check_pte(unsigned char *mapping, unsigned long virt_pte,
		     unsigned long file_pte)
{
	uint64_t actual;
	uint64_t expected = pattern(file_pte);

	memcpy(&actual, mapping + virt_pte * PTE_SIZE_BYTES, sizeof(actual));
	if (actual == expected)
		return 0;

	fprintf(stderr,
		"mismatch virtual_pte=%lu expected_file_pte=%lu expected=%016lx actual=%016lx\n",
		virt_pte, file_pte, (unsigned long)expected,
		(unsigned long)actual);
	return -1;
}

int main(void)
{
	const size_t length = FILE_PTES * PTE_SIZE_BYTES;
	unsigned char *mapping;
	char path[] = "/tmp/remap-subpg-XXXXXX";
	unsigned long i;
	int fd, rc = EXIT_FAILURE;

	fd = mkstemp(path);
	if (fd < 0) {
		perror("mkstemp");
		return EXIT_FAILURE;
	}
	unlink(path);

	if (ftruncate(fd, length)) {
		perror("ftruncate");
		goto out_close;
	}

	for (i = 0; i < FILE_PTES; i++) {
		uint64_t value = pattern(i);

		if (pwrite(fd, &value, sizeof(value),
			   i * PTE_SIZE_BYTES) != sizeof(value)) {
			perror("pwrite");
			goto out_close;
		}
	}

	mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		goto out_close;
	}

	errno = 0;
	if (syscall(SYS_remap_file_pages,
		    mapping + REMAP_FIRST * PTE_SIZE_BYTES,
		    REMAP_PTES * PTE_SIZE_BYTES, 0, REMAP_PGOFF, 0)) {
		perror("remap_file_pages");
		goto out_unmap;
	}

	if (check_pte(mapping, REMAP_FIRST - 1, REMAP_FIRST - 1))
		goto out_unmap;
	for (i = 0; i < REMAP_PTES; i++) {
		if (check_pte(mapping, REMAP_FIRST + i, REMAP_PGOFF + i))
			goto out_unmap;
	}
	if (check_pte(mapping, REMAP_FIRST + REMAP_PTES,
		      REMAP_FIRST + REMAP_PTES))
		goto out_unmap;

	puts("REMAP_FILE_PAGES_SUBPG_PASS");
	rc = EXIT_SUCCESS;
out_unmap:
	munmap(mapping, length);
out_close:
	close(fd);
	return rc;
}
