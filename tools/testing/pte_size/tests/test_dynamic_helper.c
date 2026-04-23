/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal dynamically linked helper for test_dynamic.  Linked with
 * libm so that ld-linux must load a second DSO beyond libc, which is
 * what triggers the MAP_PRIVATE fault-around batching bug on
 * PG_SIZE > PTE_SIZE kernels.
 */
#include <math.h>

int main(int argc, char *argv[])
{
	volatile double x = 1.0;

	/* Pull in libm so the loader actually maps and relocates it. */
	x = sqrt(x) + cos(x);
	if (x < 0.0)
		return 1;

	return 42;
}
