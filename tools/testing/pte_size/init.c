/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal init for PTE_SIZE page fault testing.
 *
 * Uses raw syscalls to avoid musl's malloc/mmap which can hang
 * on kernels with PG_SIZE > PTE_SIZE bugs.
 *
 * Mounts /proc, /sys, /dev, /tmp, then runs each test_* binary
 * in /tests/ sequentially. Reports pass/fail. Powers off when done.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <signal.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_DIR "/tests"
#define MAX_TESTS 64
#define MAX_NAME  64

/*
 * All output via write(2) to fd 1/2 — no stdio buffering, no malloc.
 */
static void puts_fd(int fd, const char *s)
{
	write(fd, s, strlen(s));
}

static void puts_out(const char *s) { puts_fd(1, s); }
static void puts_err(const char *s) { puts_fd(2, s); }

static void put_int(int fd, int n)
{
	char buf[16];
	int i = sizeof(buf) - 1;

	if (n < 0) {
		write(fd, "-", 1);
		n = -n;
	}
	buf[i] = '\0';
	do {
		buf[--i] = '0' + (n % 10);
		n /= 10;
	} while (n > 0);
	write(fd, buf + i, sizeof(buf) - 1 - i);
}

/* Linux getdents64 structure */
struct linux_dirent64 {
	unsigned long long d_ino;
	long long d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

static int run_test(const char *path)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		execl(path, path, (char *)NULL);
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0)
		return -1;

	if (WIFEXITED(status))
		return WEXITSTATUS(status);

	if (WIFSIGNALED(status)) {
		puts_err("  killed by signal ");
		put_int(2, WTERMSIG(status));
		puts_err("\n");
		return -1;
	}

	return -1;
}

/* Simple bubble sort for test names */
static void sort_names(char names[][MAX_NAME], int count)
{
	for (int i = 0; i < count - 1; i++) {
		for (int j = 0; j < count - 1 - i; j++) {
			if (strcmp(names[j], names[j+1]) > 0) {
				char tmp[MAX_NAME];
				memcpy(tmp, names[j], MAX_NAME);
				memcpy(names[j], names[j+1], MAX_NAME);
				memcpy(names[j+1], tmp, MAX_NAME);
			}
		}
	}
}

int main(void)
{
	char path[512];
	char buf[4096]; /* for getdents64 and file reads */
	int pass = 0, fail = 0, total = 0;

	/* We are PID 1 — mount essential filesystems */
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
	mkdir("/dev/pts", 0755);
	mount("devpts", "/dev/pts", "devpts", 0, NULL);

	puts_out("\n========================================\n");
	puts_out("  PTE_SIZE page fault test suite\n");
	puts_out("========================================\n\n");

	/* Read /proc/version */
	{
		int fd = open("/proc/version", O_RDONLY);
		if (fd >= 0) {
			ssize_t n = read(fd, buf, sizeof(buf) - 1);
			if (n > 0) {
				buf[n] = '\0';
				puts_out("Kernel: ");
				puts_out(buf);
				puts_out("\n");
			}
			close(fd);
		}
	}

	/* Read /proc/cmdline */
	{
		int fd = open("/proc/cmdline", O_RDONLY);
		if (fd >= 0) {
			ssize_t n = read(fd, buf, sizeof(buf) - 1);
			if (n > 0) {
				buf[n] = '\0';
				puts_out("Cmdline: ");
				puts_out(buf);
			}
			close(fd);
		}
	}

	/* Scan /tests using raw getdents64 syscall */
	static char names[MAX_TESTS][MAX_NAME];
	int count = 0;

	puts_out("[init] scanning /tests...\n");

	int dfd = open(TEST_DIR, O_RDONLY | O_DIRECTORY);
	if (dfd < 0) {
		puts_err("Cannot open " TEST_DIR "\n");
		goto done;
	}

	puts_out("[init] getdents64...\n");

	for (;;) {
		long nread = syscall(SYS_getdents64, dfd, buf, sizeof(buf));
		if (nread <= 0)
			break;

		for (long pos = 0; pos < nread; ) {
			struct linux_dirent64 *d =
				(struct linux_dirent64 *)(buf + pos);
			const char *name = d->d_name;

			if (name[0] != '.' &&
			    strncmp(name, "test_", 5) == 0 &&
			    strstr(name, "_helper") == NULL &&
			    count < MAX_TESTS) {
				strncpy(names[count], name, MAX_NAME - 1);
				names[count][MAX_NAME - 1] = '\0';
				count++;
			}
			pos += d->d_reclen;
		}
	}
	close(dfd);

	sort_names(names, count);

	puts_out("\nFound ");
	put_int(1, count);
	puts_out(" tests\n\n");

	for (int i = 0; i < count; i++) {
		int ret;

		strcpy(path, TEST_DIR "/");
		strcat(path, names[i]);
		total++;

		puts_out("[");
		put_int(1, total);
		puts_out("/");
		put_int(1, count);
		puts_out("] ");
		puts_out(names[i]);
		/* Pad to ~40 chars */
		for (int pad = 40 - (int)strlen(names[i]); pad > 0; pad--)
			write(1, " ", 1);
		puts_out(" ");

		ret = run_test(path);
		if (ret == 0) {
			puts_out("\033[32mPASS\033[0m\n");
			pass++;
		} else {
			puts_out("\033[31mFAIL\033[0m (exit=");
			put_int(1, ret);
			puts_out(")\n");
			fail++;
		}
	}

done:
	puts_out("\n========================================\n");
	puts_out("  Results: ");
	put_int(1, pass);
	puts_out(" passed, ");
	put_int(1, fail);
	puts_out(" failed, ");
	put_int(1, total);
	puts_out(" total\n");
	puts_out("========================================\n\n");

	sync();
	reboot(RB_POWER_OFF);
	return 0;
}
