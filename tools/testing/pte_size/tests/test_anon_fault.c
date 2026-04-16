/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test anonymous page faults.
 *
 * Exercises do_anonymous_page() with various mmap sizes and access
 * patterns. With PG_SIZE > PTE_SIZE, a single order-0 folio must
 * populate multiple PTEs.
 */
#include "test_common.h"

static int test_small_anon(void)
{
	char *p;
	size_t len = KB(64);

	TEST_LOG("anonymous mmap %zu bytes, write every 4K", len);
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	touch_range_write(p, len);
	ASSERT(verify_range(p, len, 0x42, 4096) == 0, "data mismatch");

	munmap(p, len);
	return 0;
}

static int test_large_anon(void)
{
	char *p;
	size_t len = MB(4);

	TEST_LOG("anonymous mmap %zu bytes, write every 4K", len);
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	touch_range_write(p, len);
	ASSERT(verify_range(p, len, 0x42, 4096) == 0, "data mismatch");

	munmap(p, len);
	return 0;
}

static int test_anon_read_then_write(void)
{
	volatile char *p;
	size_t len = KB(64);

	TEST_LOG("anonymous read (zero page) then write (COW)");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* First read should get the zero page */
	touch_range_read(p, len);
	ASSERT(verify_range(p, len, 0, 4096) == 0, "zero page not zero");

	/* Write should trigger COW from zero page */
	touch_range_write((char *)p, len);
	ASSERT(verify_range(p, len, 0x42, 4096) == 0, "data mismatch");

	munmap((void *)p, len);
	return 0;
}

static int test_anon_unaligned_size(void)
{
	char *p;
	/* Use a size that is not a multiple of 64K (PG_SIZE) */
	size_t len = KB(64) + KB(4) + 37;

	TEST_LOG("anonymous mmap with odd size %zu", len);
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Write to first and last bytes */
	p[0] = 'A';
	p[len - 1] = 'Z';
	ASSERT(p[0] == 'A', "first byte");
	ASSERT(p[len - 1] == 'Z', "last byte");

	munmap(p, len);
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_small_anon();
	ret |= test_large_anon();
	ret |= test_anon_read_then_write();
	ret |= test_anon_unaligned_size();

	return ret ? 1 : 0;
}
