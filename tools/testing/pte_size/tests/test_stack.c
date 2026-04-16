/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Test stack growth page faults.
 *
 * The stack VMA grows downward on demand. With PTE_SIZE != PG_SIZE,
 * the expand_stack / fault path must handle the size mismatch.
 */
#include "test_common.h"

static volatile int depth_reached;

static void recurse(int depth, int target)
{
	/* ~4K of stack per frame (local array) */
	volatile char buf[3072];

	buf[0] = depth & 0xFF;
	buf[sizeof(buf) - 1] = depth & 0xFF;

	if (depth >= target) {
		depth_reached = depth;
		return;
	}

	recurse(depth + 1, target);
}

static int test_stack_growth(void)
{
	/*
	 * 256 frames * ~4K = ~1MB of stack growth.
	 * Default stack limit is 8MB, so this should be fine.
	 */
	int target = 256;

	TEST_LOG("stack growth: %d frames (~1MB)", target);
	depth_reached = 0;
	recurse(0, target);
	ASSERT(depth_reached == target,
	       "only reached depth %d / %d", depth_reached, target);

	return 0;
}

static int test_stack_in_child(void)
{
	pid_t pid;
	int status;

	TEST_LOG("stack growth in forked child");
	pid = fork();
	ASSERT(pid >= 0, "fork: %s", strerror(errno));

	if (pid == 0) {
		depth_reached = 0;
		recurse(0, 128);
		_exit(depth_reached == 128 ? 0 : 1);
	}

	waitpid(pid, &status, 0);
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	       "child stack growth failed");

	return 0;
}

int main(void)
{
	int ret = 0;

	ret |= test_stack_growth();
	ret |= test_stack_in_child();

	return ret ? 1 : 0;
}
