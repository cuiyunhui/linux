/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_FAIL(fmt, ...) do { \
	fprintf(stderr, "  FAIL: " fmt "\n", ##__VA_ARGS__); \
	return 1; \
} while (0)

#define TEST_LOG(fmt, ...) \
	fprintf(stderr, "  " fmt "\n", ##__VA_ARGS__)

#define ASSERT(cond, fmt, ...) do { \
	if (!(cond)) \
		TEST_FAIL(fmt, ##__VA_ARGS__); \
} while (0)

/* Sizes — note these are compile-time constants from the host, but
 * the kernel may have different PTE_SIZE / PG_SIZE. The tests are
 * designed to work with any PTE_SIZE <= PG_SIZE. */
#define KB(x) ((size_t)(x) * 1024)
#define MB(x) ((size_t)(x) * 1024 * 1024)

/* Touch every 4K offset in a range to ensure faults at PTE granularity */
static inline void touch_range_read(volatile char *base, size_t len)
{
	volatile char sink;
	for (size_t off = 0; off < len; off += 4096)
		sink = base[off];
	(void)sink;
}

static inline void touch_range_write(volatile char *base, size_t len)
{
	for (size_t off = 0; off < len; off += 4096)
		base[off] = 0x42;
}

/*
 * Verify a range is readable and contains expected byte.
 * Returns 0 on success, 1 on mismatch.
 */
static inline int verify_range(volatile char *base, size_t len,
			       char expected, size_t stride)
{
	for (size_t off = 0; off < len; off += stride) {
		if (base[off] != expected) {
			fprintf(stderr, "  mismatch at offset %zu: "
				"got 0x%02x, expected 0x%02x\n",
				off, (unsigned char)base[off],
				(unsigned char)expected);
			return 1;
		}
	}
	return 0;
}

#endif /* TEST_COMMON_H */
