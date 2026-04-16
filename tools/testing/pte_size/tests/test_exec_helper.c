/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Helper binary for test_exec.
 *
 * If called with "chain" argument, execs itself without the argument
 * (testing exec-after-exec). Otherwise just exits with code 42.
 *
 * The fact that this binary runs at all proves ELF loading worked:
 * text, data, and bss segments were mapped and faulted in correctly.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Put something in .data and .bss to exercise those segment faults */
static int data_var = 0xDEAD;
static int bss_var;

int main(int argc, char *argv[])
{
	/* Verify data segment loaded correctly */
	if (data_var != 0xDEAD)
		return 1;

	/* Verify bss is zeroed */
	if (bss_var != 0)
		return 2;

	/* Use bss to prevent optimization */
	bss_var = 1;

	if (argc > 1 && strcmp(argv[1], "chain") == 0) {
		/* Re-exec without "chain" argument */
		execl("/tests/test_exec_helper", "test_exec_helper", NULL);
		return 127;
	}

	return 42;
}
