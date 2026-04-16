/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test munmap and partial unmapping.
 *
 * With PTE_SIZE != PG_SIZE, partial unmaps that split a folio's
 * PTE range are especially interesting.
 */
#include "test_common.h"
#include <sys/syscall.h>

/* Use write(2) directly for debug markers to avoid any buffering */
static void marker(const char *s)
{
	write(2, s, strlen(s));
}

static int test_munmap_middle(void)
{
	char *p;
	size_t len = KB(192); /* 3 x 64K */

	marker("[munmap_middle] mmap 192K...\n");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	marker("[munmap_middle] memset...\n");
	memset(p, 0xAA, len);

	marker("[munmap_middle] munmap middle 64K...\n");
	/* Unmap the middle 64K */
	int ret = munmap(p + KB(64), KB(64));
	marker("[munmap_middle] munmap returned\n");
	ASSERT(ret == 0, "munmap: %s", strerror(errno));

	marker("[munmap_middle] read first region...\n");
	/* First and last 64K should still be accessible */
	ASSERT((unsigned char)p[0] == 0xAA, "first region");
	marker("[munmap_middle] read last region...\n");
	ASSERT((unsigned char)p[KB(128)] == 0xAA, "last region");

	marker("[munmap_middle] cleanup...\n");
	munmap(p, KB(64));
	munmap(p + KB(128), KB(64));
	marker("[munmap_middle] done\n");
	return 0;
}

static int test_munmap_head(void)
{
	char *p;
	size_t len = KB(128);

	marker("[munmap_head] mmap 128K...\n");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	marker("[munmap_head] memset...\n");
	memset(p, 0xBB, len);

	marker("[munmap_head] munmap head 64K...\n");
	int ret = munmap(p, KB(64));
	marker("[munmap_head] munmap returned\n");
	ASSERT(ret == 0, "munmap: %s", strerror(errno));

	marker("[munmap_head] read tail...\n");
	/* Remaining tail should be fine */
	ASSERT((unsigned char)p[KB(64)] == 0xBB, "tail region");

	marker("[munmap_head] cleanup...\n");
	munmap(p + KB(64), KB(64));
	marker("[munmap_head] done\n");
	return 0;
}

static int test_munmap_tail(void)
{
	char *p;
	size_t len = KB(128);

	marker("[munmap_tail] mmap 128K...\n");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	marker("[munmap_tail] memset...\n");
	memset(p, 0xCC, len);

	marker("[munmap_tail] munmap tail 64K...\n");
	int ret = munmap(p + KB(64), KB(64));
	marker("[munmap_tail] munmap returned\n");
	ASSERT(ret == 0, "munmap: %s", strerror(errno));

	marker("[munmap_tail] read head...\n");
	/* Head should be fine */
	ASSERT((unsigned char)p[0] == 0xCC, "head region");

	marker("[munmap_tail] cleanup...\n");
	munmap(p, KB(64));
	marker("[munmap_tail] done\n");
	return 0;
}

static int test_munmap_4k_granular(void)
{
	char *p;
	size_t len = KB(64);

	marker("[munmap_4k] mmap 64K...\n");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	marker("[munmap_4k] memset...\n");
	memset(p, 0xDD, len);

	marker("[munmap_4k] munmap first 4K...\n");
	/* Unmap just the first 4K */
	int ret = munmap(p, KB(4));
	marker("[munmap_4k] munmap returned\n");
	ASSERT(ret == 0, "munmap 4K: %s", strerror(errno));

	marker("[munmap_4k] read p+4K...\n");
	/* p+4K onwards should still be valid */
	ASSERT((unsigned char)p[KB(4)] == 0xDD, "post-4K unmap");

	marker("[munmap_4k] cleanup: munmap(p+4K, 60K)...\n");
	ret = munmap(p + KB(4), len - KB(4));
	marker("[munmap_4k] cleanup returned ");
	{
		char tmp[32];
		int i = 0;
		if (ret < 0) { tmp[i++] = '-'; tmp[i++] = '1'; }
		else { tmp[i++] = '0'; }
		tmp[i++] = '\n';
		tmp[i] = '\0';
		marker(tmp);
	}
	marker("[munmap_4k] done\n");
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_munmap_middle();
	ret |= test_munmap_head();
	ret |= test_munmap_tail();
	ret |= test_munmap_4k_granular();

	return ret ? 1 : 0;
}
