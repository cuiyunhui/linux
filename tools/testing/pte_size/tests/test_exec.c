/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test exec / ELF loading page faults.
 *
 * The exec path uses do_mmap / elf_map which sets up VMAs for text,
 * data, bss segments. With PTE_SIZE != PG_SIZE, the ELF loader must
 * handle alignment correctly.
 *
 * This test verifies exec works by execing a helper that prints a
 * magic string. The init harness detects pass/fail from exit code.
 *
 * Since we're in a minimal initrd, we exec /tests/test_exec_helper.
 */
#include "test_common.h"

static int test_exec_basic(void)
{
	pid_t pid;
	int status;

	TEST_LOG("exec helper binary");
	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));

	if (pid == 0) {
		execl("/tests/test_exec_helper", "test_exec_helper", NULL);
		/* If we get here, exec failed */
		perror("execl");
		_exit(127);
	}

	waitpid(pid, &status, 0);
	ASSERT(WIFEXITED(status), "helper killed by signal %d",
	       WIFSIGNALED(status) ? WTERMSIG(status) : -1);
	ASSERT(WEXITSTATUS(status) == 42,
	       "helper exit code: %d (expected 42)", WEXITSTATUS(status));

	return 0;
}

static int test_exec_chain(void)
{
	pid_t pid;
	int status;

	TEST_LOG("exec chain (exec replaces exec)");
	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));

	if (pid == 0) {
		/* exec helper which should exec itself is simple enough;
		 * just exec the helper to verify two execs in a row work */
		execl("/tests/test_exec_helper", "test_exec_helper",
		      "chain", NULL);
		_exit(127);
	}

	waitpid(pid, &status, 0);
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 42,
	       "chain exec failed: %d", WEXITSTATUS(status));

	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_exec_basic();
	ret |= test_exec_chain();

	return ret ? 1 : 0;
}
