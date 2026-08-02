// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include "test_common.h"

#include <linux/memfd.h>
#include <setjmp.h>

#define PTE_SIZE_BYTES		4096UL
#define SHMEM_MAPPING_SIZE	(16UL * PTE_SIZE_BYTES)
#define SHMEM_TRUNCATE_SIZE	(2UL * PTE_SIZE_BYTES)

static sigjmp_buf sigbus_env;

static void sigbus_handler(int sig)
{
	(void)sig;
	siglongjmp(sigbus_env, 1);
}

static unsigned char load_byte(const unsigned char *addr)
{
	return __atomic_load_n(addr, __ATOMIC_RELAXED);
}

static int expect_sigbus(const unsigned char *addr)
{
	if (!sigsetjmp(sigbus_env, 1)) {
		unsigned char value = load_byte(addr);

		(void)value;
		return 0;
	}
	return 1;
}

static int fill_file(int fd)
{
	unsigned char buf[PTE_SIZE_BYTES];
	size_t pte;

	if (ftruncate(fd, SHMEM_MAPPING_SIZE))
		return -1;

	for (pte = 0; pte < SHMEM_MAPPING_SIZE / PTE_SIZE_BYTES; pte++) {
		memset(buf, (unsigned char)(pte + 1), sizeof(buf));
		if (pwrite(fd, buf, sizeof(buf), pte * sizeof(buf)) !=
		    sizeof(buf))
			return -1;
	}
	return 0;
}

static int test_mapping(int fd, int flags, const char *name)
{
	unsigned char *mapping;
	unsigned char sink = 0;
	size_t i;

	if (fill_file(fd))
		TEST_FAIL("%s fill file: %s", name, strerror(errno));

	mapping = mmap(NULL, SHMEM_MAPPING_SIZE, PROT_READ, flags, fd, 0);
	if (mapping == MAP_FAILED)
		TEST_FAIL("%s mmap: %s", name, strerror(errno));

	for (i = 0; i < SHMEM_MAPPING_SIZE; i += PTE_SIZE_BYTES)
		sink ^= load_byte(mapping + i);

	if (ftruncate(fd, SHMEM_TRUNCATE_SIZE))
		TEST_FAIL("%s ftruncate: %s", name, strerror(errno));

	ASSERT(load_byte(mapping + PTE_SIZE_BYTES) == 2,
	       "%s last in-range PTE changed after truncate", name);
	ASSERT(expect_sigbus(mapping + SHMEM_TRUNCATE_SIZE),
	       "%s first PTE beyond EOF did not SIGBUS", name);
	(void)sink;

	if (munmap(mapping, SHMEM_MAPPING_SIZE))
		TEST_FAIL("%s munmap: %s", name, strerror(errno));
	return 0;
}

int main(void)
{
	struct sigaction act = {
		.sa_handler = sigbus_handler,
	};
	long page_size = sysconf(_SC_PAGESIZE);
	int fd;

	if (page_size != PTE_SIZE_BYTES) {
		printf("SHMEM_TRUNCATE_SUBPG_SKIP pagesize=%ld\n", page_size);
		return EXIT_SUCCESS;
	}

	sigemptyset(&act.sa_mask);
	if (sigaction(SIGBUS, &act, NULL))
		TEST_FAIL("sigaction: %s", strerror(errno));

	fd = memfd_create("shmem-truncate-subpg", MFD_CLOEXEC);
	if (fd < 0)
		TEST_FAIL("memfd_create: %s", strerror(errno));

	if (test_mapping(fd, MAP_SHARED, "MAP_SHARED") ||
	    test_mapping(fd, MAP_PRIVATE, "MAP_PRIVATE")) {
		close(fd);
		return EXIT_FAILURE;
	}

	close(fd);
	puts("SHMEM_TRUNCATE_SUBPG_PASS");
	return EXIT_SUCCESS;
}
