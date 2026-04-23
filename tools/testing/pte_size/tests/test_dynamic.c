/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test execution of a dynamically linked binary.
 *
 * The static tests never load ld-linux.so, so they don't cover:
 *   - ELF interpreter load (PT_INTERP)
 *   - Shared library mmap + relocation
 *   - COW on .data/.got via PROT_WRITE after initial read fault
 *   - glibc TLS setup via set_thread_area/arch_prctl
 *
 * This test exec's a glibc-linked helper. If the 16K boot bug is
 * reproducible in a self-test, this is where it shows up.
 */
#include "test_common.h"

static int test_dynamic_basic(void)
{
	pid_t pid;
	int status;

	TEST_LOG("exec glibc-dynamic helper");
	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));

	if (pid == 0) {
		execl("/tests/test_dynamic_helper",
		      "test_dynamic_helper", (char *)NULL);
		perror("execl");
		_exit(127);
	}

	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status))
		TEST_FAIL("helper killed by signal %d", WTERMSIG(status));
	ASSERT(WIFEXITED(status), "helper did not exit normally");
	ASSERT(WEXITSTATUS(status) == 42,
	       "helper exit=%d (expected 42)", WEXITSTATUS(status));
	return 0;
}

static int test_dynamic_chain(void)
{
	pid_t pid;
	int status;

	TEST_LOG("exec-after-exec of glibc-dynamic helper");
	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));

	if (pid == 0) {
		execl("/tests/test_dynamic_helper",
		      "test_dynamic_helper", "chain", (char *)NULL);
		_exit(127);
	}

	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status))
		TEST_FAIL("chain killed by signal %d", WTERMSIG(status));
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 42,
	       "chain exit=%d", WEXITSTATUS(status));
	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_dynamic_basic();
	ret |= test_dynamic_chain();

	return ret ? 1 : 0;
}
