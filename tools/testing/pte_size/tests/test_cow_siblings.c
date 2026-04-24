/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Regression test for the COW sibling merge path.
 *
 * When PG_SIZE > PTE_SIZE and a file-backed MAP_PRIVATE VMA is written
 * to at two different sub-pages of the same PG, the second write must
 * merge onto the first write's anon COW folio — otherwise the first
 * write is lost. This exercises cow_find_sibling_anon() +
 * cow_copy_merge_sibling() + install_cow_siblings().
 *
 * The test is only meaningful on PG > PTE; on 4K PG == PTE, every PTE
 * is its own PG and the sibling path is skipped. We write to 4K
 * boundaries so with 16K or 64K PG each write lands on a different sub-
 * page of the same PG. The test verifies both writes are visible after
 * all faults complete.
 */
#include "test_common.h"

static int test_two_sibling_writes(void)
{
	int fd;
	char *p;
	size_t len = KB(64);
	char buf[4096];
	int i;

	TEST_LOG("MAP_PRIVATE file + write two sub-PG siblings, both preserved");

	fd = open("/tmp/cow_siblings", O_CREAT | O_RDWR | O_TRUNC, 0644);
	ASSERT(fd >= 0, "open: %s", strerror(errno));

	memset(buf, 0xAA, sizeof(buf));
	for (size_t off = 0; off < len; off += sizeof(buf)) {
		ssize_t n = write(fd, buf, sizeof(buf));
		ASSERT(n == (ssize_t)sizeof(buf), "write: %s", strerror(errno));
	}

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	/*
	 * Write a distinct byte at each 4K boundary in the first PG. On a
	 * 16K PG this touches 4 sibling PTEs; on 64K, 16 of them. The
	 * second and later writes must find each other's anon folio via
	 * cow_find_sibling_anon() and merge — if they don't, the earlier
	 * writes get clobbered when install_cow_siblings tears them down.
	 */
	for (i = 0; i < 16 && (size_t)(i * 4096) < len; i++)
		p[i * 4096] = (char)(0xC0 + i);

	for (i = 0; i < 16 && (size_t)(i * 4096) < len; i++) {
		char got = p[i * 4096];
		char want = (char)(0xC0 + i);

		ASSERT(got == want,
		       "sibling %d: got 0x%02x want 0x%02x (merge lost)",
		       i, (unsigned char)got, (unsigned char)want);
	}

	/* Read-only bytes between our writes should still be the file data */
	for (i = 0; i < 16 && (size_t)(i * 4096 + 8) < len; i++)
		ASSERT((unsigned char)p[i * 4096 + 8] == 0xAA,
		       "read-only byte at sibling %d clobbered", i);

	munmap(p, len);
	close(fd);
	unlink("/tmp/cow_siblings");
	return 0;
}

static int test_fork_then_sibling_writes(void)
{
	int fd;
	char *p;
	size_t len = KB(64);
	char buf[4096];
	pid_t pid;
	int status;

	TEST_LOG("fork child, both parent and child COW different siblings");

	fd = open("/tmp/cow_fork", O_CREAT | O_RDWR | O_TRUNC, 0644);
	ASSERT(fd >= 0, "open: %s", strerror(errno));
	memset(buf, 0x55, sizeof(buf));
	for (size_t off = 0; off < len; off += sizeof(buf))
		write(fd, buf, sizeof(buf));

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	ASSERT(p != MAP_FAILED, "mmap: %s", strerror(errno));

	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));

	if (pid == 0) {
		/* Child writes the even siblings */
		for (int i = 0; i < 16 && (size_t)(i * 4096) < len; i += 2)
			p[i * 4096] = (char)(0xE0 + i);
		for (int i = 0; i < 16 && (size_t)(i * 4096) < len; i += 2) {
			if ((unsigned char)p[i * 4096] != (unsigned char)(0xE0 + i))
				_exit(10 + i);
		}
		_exit(0);
	}

	/* Parent writes the odd siblings */
	for (int i = 1; i < 16 && (size_t)(i * 4096) < len; i += 2)
		p[i * 4096] = (char)(0xF0 + i);

	waitpid(pid, &status, 0);
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	       "child failed: status=%d", status);

	/* Parent's odd writes should be visible */
	for (int i = 1; i < 16 && (size_t)(i * 4096) < len; i += 2)
		ASSERT((unsigned char)p[i * 4096] == (unsigned char)(0xF0 + i),
		       "parent odd sibling %d lost", i);

	/* Parent's even siblings should still read file data (child's
	 * writes were in its own address space) */
	for (int i = 0; i < 16 && (size_t)(i * 4096) < len; i += 2)
		ASSERT((unsigned char)p[i * 4096] == 0x55,
		       "parent even sibling %d saw child write (COW leak)", i);

	munmap(p, len);
	close(fd);
	unlink("/tmp/cow_fork");
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_two_sibling_writes();
	ret |= test_fork_then_sibling_writes();

	return ret ? 1 : 0;
}
