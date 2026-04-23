/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal dynamic-linked helper. Just exits with a fixed code via a
 * raw syscall, avoiding libc beyond what the runtime linker itself
 * pulls in. If this crashes, the bug is in ld-linux.so startup, not
 * in anything this program does.
 */
int main(int argc, char *argv[])
{
	register long rax __asm__("rax") = 231;	/* __NR_exit_group */
	register long rdi __asm__("rdi") = 42;
	__asm__ volatile("syscall" :: "r"(rax), "r"(rdi) : "rcx", "r11", "memory");
	return 99;
}
