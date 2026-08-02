// SPDX-License-Identifier: GPL-2.0
/*
 * Verify that mlock ranges and accounting use PTE-sized units when one
 * allocator page is mapped by multiple PTEs.
 */
#include "test_common.h"
#include <sys/resource.h>

#define PTE_BYTES	4096
#define TEST_PG_BYTES	(64 * 1024)
#define TEST_LOOPS	2000

static int read_vmlck_kb(unsigned long *vmlck_kb)
{
	char line[256];
	FILE *status;

	status = fopen("/proc/self/status", "r");
	if (!status)
		TEST_FAIL("fopen(/proc/self/status): %s", strerror(errno));

	while (fgets(line, sizeof(line), status)) {
		if (sscanf(line, "VmLck: %lu kB", vmlck_kb) == 1) {
			fclose(status);
			return 0;
		}
	}

	fclose(status);
	TEST_FAIL("VmLck not found in /proc/self/status");
}

static int expect_vmlck(unsigned long expected_kb, const char *where)
{
	unsigned long actual_kb;

	if (read_vmlck_kb(&actual_kb))
		return 1;
	if (actual_kb != expected_kb)
		TEST_FAIL("%s: VmLck=%lu kB, expected %lu kB",
			  where, actual_kb, expected_kb);
	return 0;
}

static int verify_pattern(const unsigned char *p, size_t len)
{
	unsigned char expected;
	size_t off;

	for (off = 0; off < len; off += PTE_BYTES) {
		expected = (off / PTE_BYTES) + 1;

		if (p[off] != expected)
			TEST_FAIL("content mismatch at offset %zu: 0x%02x != 0x%02x",
				  off, p[off], expected);
	}
	return 0;
}

int main(void)
{
	struct rlimit limit;
	unsigned char *map, *p;
	unsigned long baseline_kb;
	uintptr_t aligned;
	size_t off;
	int i;

	if (getrlimit(RLIMIT_MEMLOCK, &limit))
		TEST_FAIL("getrlimit(RLIMIT_MEMLOCK): %s", strerror(errno));
	if (limit.rlim_cur < TEST_PG_BYTES) {
		if (limit.rlim_max < TEST_PG_BYTES)
			TEST_FAIL("RLIMIT_MEMLOCK hard limit is below %u bytes",
				  TEST_PG_BYTES);
		limit.rlim_cur = TEST_PG_BYTES;
		if (setrlimit(RLIMIT_MEMLOCK, &limit))
			TEST_FAIL("setrlimit(RLIMIT_MEMLOCK): %s",
				  strerror(errno));
	}

	map = mmap(NULL, TEST_PG_BYTES * 2, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED)
		TEST_FAIL("mmap: %s", strerror(errno));

	aligned = ((uintptr_t)map + TEST_PG_BYTES - 1) &
		  ~(uintptr_t)(TEST_PG_BYTES - 1);
	p = (unsigned char *)aligned;

	for (off = 0; off < TEST_PG_BYTES; off += PTE_BYTES)
		p[off] = (off / PTE_BYTES) + 1;

	if (read_vmlck_kb(&baseline_kb))
		return 1;

	TEST_LOG("mlock a non-first 4K PTE inside a 64K-aligned range");
	if (mlock(p + PTE_BYTES, PTE_BYTES))
		TEST_FAIL("partial mlock: %s", strerror(errno));
	if (expect_vmlck(baseline_kb + PTE_BYTES / 1024, "partial mlock"))
		return 1;
	if (munlock(p + PTE_BYTES, PTE_BYTES))
		TEST_FAIL("partial munlock: %s", strerror(errno));
	if (expect_vmlck(baseline_kb, "partial munlock"))
		return 1;

	TEST_LOG("mlock all PTEs backed by one 64K allocator page");
	if (mlock(p, TEST_PG_BYTES))
		TEST_FAIL("full mlock: %s", strerror(errno));
	if (expect_vmlck(baseline_kb + TEST_PG_BYTES / 1024, "full mlock"))
		return 1;
	if (munlock(p, TEST_PG_BYTES))
		TEST_FAIL("full munlock: %s", strerror(errno));
	if (expect_vmlck(baseline_kb, "full munlock"))
		return 1;

	TEST_LOG("repeat full-folio mlock/munlock without duplicate processing");
	for (i = 0; i < TEST_LOOPS; i++) {
		if (mlock(p, TEST_PG_BYTES))
			TEST_FAIL("mlock loop %d: %s", i, strerror(errno));
		if (munlock(p, TEST_PG_BYTES))
			TEST_FAIL("munlock loop %d: %s", i, strerror(errno));
	}

	if (expect_vmlck(baseline_kb, "mlock loop"))
		return 1;
	if (verify_pattern(p, TEST_PG_BYTES))
		return 1;

	munmap(map, TEST_PG_BYTES * 2);
	return 0;
}
