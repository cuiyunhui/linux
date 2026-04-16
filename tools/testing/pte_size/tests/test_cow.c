/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test copy-on-write page faults.
 *
 * Exercises wp_page_copy() / do_wp_page(). With PG_SIZE > PTE_SIZE,
 * a COW fault on one PTE must properly handle the rest of the PTEs
 * mapping the same folio.
 */
#include "test_common.h"

static int test_cow_basic(void)
{
	char *p;
	size_t len = KB(64);
	pid_t pid;
	int status;

	TEST_LOG("basic COW: fork + write");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/* Parent writes pattern */
	memset(p, 0xAA, len);

	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));

	if (pid == 0) {
		/* Child: verify parent's data, then write new pattern */
		for (size_t i = 0; i < len; i += 4096) {
			if ((unsigned char)p[i] != 0xAA)
				_exit(1);
		}
		/* Trigger COW */
		memset(p, 0xBB, len);
		for (size_t i = 0; i < len; i += 4096) {
			if ((unsigned char)p[i] != 0xBB)
				_exit(2);
		}
		_exit(0);
	}

	waitpid(pid, &status, 0);
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	       "child failed: %d", WEXITSTATUS(status));

	/* Parent data should be unchanged */
	ASSERT(verify_range(p, len, (char)0xAA, 4096) == 0,
	       "parent data corrupted after child COW");

	munmap(p, len);
	return 0;
}

static int test_cow_partial_write(void)
{
	char *p;
	size_t len = KB(128);
	pid_t pid;
	int status;

	TEST_LOG("partial COW: child writes only first 4K of each 64K");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	memset(p, 0xCC, len);

	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));

	if (pid == 0) {
		/*
		 * Write only one 4K chunk out of each 64K region.
		 * This tests that COW correctly copies the entire
		 * PG_SIZE folio even when only one PTE faults.
		 */
		for (size_t off = 0; off < len; off += KB(64))
			p[off] = 0xDD;

		/* Verify: written bytes changed, others untouched */
		for (size_t off = 0; off < len; off += KB(64)) {
			if ((unsigned char)p[off] != 0xDD)
				_exit(1);
			/* The rest of the folio should still be 0xCC */
			if (off + 4096 < len &&
			    (unsigned char)p[off + 4096] != 0xCC)
				_exit(2);
		}
		_exit(0);
	}

	waitpid(pid, &status, 0);
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	       "child failed: %d", WEXITSTATUS(status));

	/* Parent should be entirely 0xCC still */
	ASSERT(verify_range(p, len, (char)0xCC, 4096) == 0,
	       "parent data corrupted");

	munmap(p, len);
	return 0;
}

static int test_cow_multiple_forks(void)
{
	char *p;
	size_t len = KB(64);
	int i;

	TEST_LOG("COW chain: 4 sequential forks writing same region");
	p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	memset(p, 0, len);

	for (i = 1; i <= 4; i++) {
		pid_t pid;
		int status;

		pid = fork();
		ASSERT(pid >= 0, "fork %d: %s", i, strerror(errno));

		if (pid == 0) {
			memset(p, i, len);
			for (size_t off = 0; off < len; off += 4096) {
				if ((unsigned char)p[off] != (unsigned char)i)
					_exit(1);
			}
			_exit(0);
		}

		waitpid(pid, &status, 0);
		ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		       "child %d failed", i);
	}

	/* Parent should still be all zeros */
	ASSERT(verify_range(p, len, 0, 4096) == 0,
	       "parent data corrupted after COW chain");

	munmap(p, len);
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_cow_basic();
	ret |= test_cow_partial_write();
	ret |= test_cow_multiple_forks();

	return ret ? 1 : 0;
}
