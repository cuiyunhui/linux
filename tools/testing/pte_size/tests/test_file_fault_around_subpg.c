// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include "test_common.h"

#include <linux/magic.h>
#include <setjmp.h>
#include <sys/resource.h>
#include <sys/vfs.h>

#define PTE_SIZE_BYTES	4096UL
#define FAULT_AROUND_SIZE	(16UL * PTE_SIZE_BYTES)
#define MAPPING_SIZE		(2UL * FAULT_AROUND_SIZE)
#define MAX_NEIGHBOR_FAULTS	2L

static sigjmp_buf sigbus_env;

static void sigbus_handler(int sig)
{
	(void)sig;
	siglongjmp(sigbus_env, 1);
}

static long minor_faults(void)
{
	struct rusage usage;

	if (getrusage(RUSAGE_SELF, &usage))
		return -1;
	return usage.ru_minflt;
}

static unsigned char load_byte(const unsigned char *addr)
{
	return __atomic_load_n(addr, __ATOMIC_RELAXED);
}

static int fill_file(int fd)
{
	unsigned char buf[PTE_SIZE_BYTES];
	size_t pte;

	for (pte = 0; pte < MAPPING_SIZE / PTE_SIZE_BYTES; pte++) {
		memset(buf, (unsigned char)(pte + 1), sizeof(buf));
		if (pwrite(fd, buf, sizeof(buf), pte * sizeof(buf)) !=
		    sizeof(buf))
			return -1;
	}
	return 0;
}

static int test_fault_around(int fd)
{
	unsigned char *mapping;
	unsigned char *probe;
	uintptr_t aligned;
	long before, after;
	unsigned char sink = 0;
	size_t i;

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mapping == MAP_FAILED)
		TEST_FAIL("fault-around mmap: %s", strerror(errno));

	aligned = ((uintptr_t)mapping + FAULT_AROUND_SIZE - 1) &
		  ~(FAULT_AROUND_SIZE - 1);
	probe = (unsigned char *)aligned;
	ASSERT(probe + FAULT_AROUND_SIZE <= mapping + MAPPING_SIZE,
	       "no aligned fault-around window in mapping");

	/* Warm getrusage() itself before collecting the fault delta. */
	ASSERT(minor_faults() >= 0, "initial getrusage failed");
	sink ^= load_byte(probe);
	before = minor_faults();
	ASSERT(before >= 0, "getrusage after first fault failed");

	for (i = PTE_SIZE_BYTES; i < FAULT_AROUND_SIZE;
	     i += PTE_SIZE_BYTES)
		sink ^= load_byte(probe + i);

	after = minor_faults();
	ASSERT(after >= 0, "final getrusage failed");
	ASSERT(after - before <= MAX_NEIGHBOR_FAULTS,
	       "neighbor accesses caused %ld minor faults, fault-around inactive",
	       after - before);
	(void)sink;

	if (munmap((void *)mapping, MAPPING_SIZE))
		TEST_FAIL("fault-around munmap: %s", strerror(errno));
	return 0;
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

static int test_truncate_boundary(int fd)
{
	struct sigaction act = {
		.sa_handler = sigbus_handler,
	};
	unsigned char *mapping;
	unsigned char sink = 0;
	size_t i;

	sigemptyset(&act.sa_mask);
	if (sigaction(SIGBUS, &act, NULL))
		TEST_FAIL("sigaction: %s", strerror(errno));

	mapping = mmap(NULL, FAULT_AROUND_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mapping == MAP_FAILED)
		TEST_FAIL("truncate mmap: %s", strerror(errno));

	/* Ensure fault-around has installed PTEs beyond the future EOF. */
	for (i = 0; i < FAULT_AROUND_SIZE; i += PTE_SIZE_BYTES)
		sink ^= load_byte(mapping + i);

	if (ftruncate(fd, 2 * PTE_SIZE_BYTES))
		TEST_FAIL("ftruncate: %s", strerror(errno));

	sink ^= load_byte(mapping + PTE_SIZE_BYTES);
	ASSERT(expect_sigbus(mapping + 2 * PTE_SIZE_BYTES),
	       "access at truncated PTE boundary did not SIGBUS");
	(void)sink;

	if (munmap((void *)mapping, FAULT_AROUND_SIZE))
		TEST_FAIL("truncate munmap: %s", strerror(errno));
	return 0;
}

int main(void)
{
	char path[] = "./file-fault-around-XXXXXX";
	struct statfs fs;
	long page_size = sysconf(_SC_PAGESIZE);
	int fd;

	if (page_size != PTE_SIZE_BYTES) {
		printf("FILE_FAULT_AROUND_SUBPG_SKIP pagesize=%ld\n", page_size);
		return EXIT_SUCCESS;
	}

	fd = mkstemp(path);
	if (fd < 0)
		TEST_FAIL("mkstemp: %s", strerror(errno));
	unlink(path);

	if (fstatfs(fd, &fs))
		TEST_FAIL("fstatfs: %s", strerror(errno));
	if (fs.f_type == TMPFS_MAGIC) {
		close(fd);
		puts("FILE_FAULT_AROUND_SUBPG_SKIP tmpfs backing file");
		return EXIT_SUCCESS;
	}

	if (fill_file(fd))
		TEST_FAIL("fill file: %s", strerror(errno));
	if (test_fault_around(fd))
		return EXIT_FAILURE;
	if (test_truncate_boundary(fd))
		return EXIT_FAILURE;

	close(fd);
	puts("FILE_FAULT_AROUND_SUBPG_PASS");
	return EXIT_SUCCESS;
}
