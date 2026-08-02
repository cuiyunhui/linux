// SPDX-License-Identifier: GPL-2.0
#include "test_common.h"
#include "svnapot_probe/svnapot_probe_uapi.h"

#include <sys/ioctl.h>

#define PTE_BYTES	4096UL
#define NAPOT_BYTES	SVNAPOT_PROBE_SIZE
#define FULL_MASK	((1U << SVNAPOT_PROBE_PTES) - 1)
#define RISCV_PTE_AD	((1ULL << 6) | (1ULL << 7))

struct aligned_map {
	unsigned char *raw;
	unsigned char *addr;
	size_t len;
};

static int reserve_aligned(struct aligned_map *map)
{
	uintptr_t aligned;

	map->len = NAPOT_BYTES * 3;
	map->raw = mmap(NULL, map->len, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map->raw == MAP_FAILED)
		TEST_FAIL("reserve mmap: %s", strerror(errno));

	aligned = ((uintptr_t)map->raw + NAPOT_BYTES - 1) &
		  ~(uintptr_t)(NAPOT_BYTES - 1);
	map->addr = (unsigned char *)aligned;
	return 0;
}

static void release_aligned(struct aligned_map *map)
{
	if (map->raw != MAP_FAILED)
		munmap(map->raw, map->len);
}

static int map_anon(struct aligned_map *map, int prot)
{
	if (reserve_aligned(map))
		return 1;
	if (mprotect(map->addr, NAPOT_BYTES, prot)) {
		release_aligned(map);
		TEST_FAIL("anonymous mprotect: %s", strerror(errno));
	}
	return 0;
}

static int map_file(struct aligned_map *map, int fd, int prot, int flags)
{
	void *ret;

	if (reserve_aligned(map))
		return 1;
	ret = mmap(map->addr, NAPOT_BYTES, prot, flags | MAP_FIXED, fd, 0);
	if (ret == MAP_FAILED) {
		release_aligned(map);
		TEST_FAIL("file mmap: %s", strerror(errno));
	}
	return 0;
}

static void fill_slots(unsigned char *addr, unsigned char seed)
{
	size_t i;

	for (i = 0; i < SVNAPOT_PROBE_PTES; i++)
		addr[i * PTE_BYTES] = seed + i;
}

static int verify_slots(unsigned char *addr, unsigned char seed)
{
	size_t i;

	for (i = 0; i < SVNAPOT_PROBE_PTES; i++) {
		if (addr[i * PTE_BYTES] != (unsigned char)(seed + i))
			TEST_FAIL("slot %zu got 0x%x expected 0x%x", i,
				  addr[i * PTE_BYTES],
				  (unsigned int)(seed + i));
	}
	return 0;
}

static int query_ptes(int fd, void *addr, struct svnapot_probe_query *query)
{
	memset(query, 0, sizeof(*query));
	query->addr = (uintptr_t)addr;
	if (ioctl(fd, SVNAPOT_PROBE_QUERY, query))
		TEST_FAIL("SVNAPOT_PROBE_QUERY: %s", strerror(errno));
	return 0;
}

static int check_consecutive_pfns(const struct svnapot_probe_query *query)
{
	unsigned int i;

	for (i = 1; i < SVNAPOT_PROBE_PTES; i++) {
		if (query->pfn[i] != query->pfn[0] + i)
			TEST_FAIL("PFN[%u]=0x%llx, expected 0x%llx", i,
				  (unsigned long long)query->pfn[i],
				  (unsigned long long)(query->pfn[0] + i));
	}
	return 0;
}

static int check_folded(const struct svnapot_probe_query *query)
{
	unsigned int i;
	uint64_t value;

	ASSERT(query->seen_mask == FULL_MASK, "seen mask 0x%x",
	       query->seen_mask);
	ASSERT(query->present_mask == FULL_MASK, "present mask 0x%x",
	       query->present_mask);
	ASSERT(query->napot_mask == FULL_MASK, "NAPOT mask 0x%x",
	       query->napot_mask);
	if (check_consecutive_pfns(query))
		return 1;

	value = query->raw[0] & ~RISCV_PTE_AD;
	for (i = 1; i < SVNAPOT_PROBE_PTES; i++)
		ASSERT((query->raw[i] & ~RISCV_PTE_AD) == value,
		       "raw PTE %u differs outside A/D bits", i);
	return 0;
}

static int check_unfolded(const struct svnapot_probe_query *query)
{
	ASSERT(query->seen_mask == FULL_MASK, "seen mask 0x%x",
	       query->seen_mask);
	ASSERT(query->present_mask == FULL_MASK, "present mask 0x%x",
	       query->present_mask);
	ASSERT(query->napot_mask == 0, "partial operation left NAPOT mask 0x%x",
	       query->napot_mask);
	return check_consecutive_pfns(query);
}

static int check_group_atomic(const struct svnapot_probe_query *query)
{
	ASSERT(query->napot_mask == 0 || query->napot_mask == FULL_MASK,
	       "partial NAPOT group 0x%x", query->napot_mask);
	return 0;
}

static int pin_each_pte(int fd, unsigned char *addr, unsigned int flags)
{
	struct svnapot_probe_pin pin;
	unsigned int i;

	for (i = 0; i < SVNAPOT_PROBE_PTES; i++) {
		memset(&pin, 0, sizeof(pin));
		pin.addr = (uintptr_t)(addr + i * PTE_BYTES);
		pin.length = 1;
		pin.flags = flags;
		if (ioctl(fd, SVNAPOT_PROBE_PIN, &pin))
			TEST_FAIL("pin slot %u: %s", i, strerror(errno));
		ASSERT(pin.pinned == 1, "slot %u pinned %d pages", i,
		       pin.pinned);
	}
	return 0;
}

static int test_anon_and_mprotect(int fd)
{
	struct svnapot_probe_query query;
	struct aligned_map map = { .raw = MAP_FAILED };

	TEST_LOG("anonymous fold, fast-GUP and partial mprotect");
	if (map_anon(&map, PROT_READ | PROT_WRITE))
		return 1;
	fill_slots(map.addr, 0x10);
	if (query_ptes(fd, map.addr, &query) || check_folded(&query))
		goto fail;
	if (pin_each_pte(fd, map.addr, 0) ||
	    pin_each_pte(fd, map.addr, SVNAPOT_PROBE_PIN_WRITE))
		goto fail;

	if (mprotect(map.addr + PTE_BYTES, PTE_BYTES, PROT_READ))
		TEST_FAIL("partial mprotect: %s", strerror(errno));
	if (query_ptes(fd, map.addr, &query) || check_unfolded(&query))
		goto fail;
	if (verify_slots(map.addr, 0x10))
		goto fail;
	if (mprotect(map.addr + PTE_BYTES, PTE_BYTES,
		     PROT_READ | PROT_WRITE))
		TEST_FAIL("restore mprotect: %s", strerror(errno));
	map.addr[PTE_BYTES] = 0x11;
	release_aligned(&map);
	return 0;

fail:
	release_aligned(&map);
	return 1;
}

static int test_zero_and_unaligned(int fd)
{
	struct svnapot_probe_query query;
	struct aligned_map map = { .raw = MAP_FAILED };
	unsigned char value = 0;
	size_t i;

	TEST_LOG("zero page and unaligned VMA stay at 4K PTE granularity");
	if (map_anon(&map, PROT_READ))
		return 1;
	for (i = 0; i < SVNAPOT_PROBE_PTES; i++)
		value |= map.addr[i * PTE_BYTES];
	if (query_ptes(fd, map.addr, &query))
		goto fail;
	ASSERT(value == 0, "zero mapping returned nonzero data");
	ASSERT(query.napot_mask == 0, "zero page folded: 0x%x",
	       query.napot_mask);
	release_aligned(&map);

	map.raw = MAP_FAILED;
	if (reserve_aligned(&map))
		return 1;
	if (mprotect(map.addr + PTE_BYTES, NAPOT_BYTES - PTE_BYTES,
		     PROT_READ | PROT_WRITE))
		TEST_FAIL("unaligned mprotect: %s", strerror(errno));
	for (i = 1; i < SVNAPOT_PROBE_PTES; i++)
		map.addr[i * PTE_BYTES] = 0x30 + i;
	if (query_ptes(fd, map.addr + PTE_BYTES, &query))
		goto fail;
	ASSERT(query.napot_mask == 0, "unaligned VMA folded: 0x%x",
	       query.napot_mask);
	release_aligned(&map);
	return 0;

fail:
	release_aligned(&map);
	return 1;
}

static int create_backing_file(void)
{
	unsigned char *buf;
	char path[] = "/tmp/svnapot-file-XXXXXX";
	size_t i;
	ssize_t ret;
	int fd;

	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	unlink(path);
	if (ftruncate(fd, NAPOT_BYTES))
		goto fail;

	buf = calloc(1, NAPOT_BYTES);
	if (!buf)
		goto fail;
	for (i = 0; i < SVNAPOT_PROBE_PTES; i++)
		buf[i * PTE_BYTES] = 0x40 + i;
	ret = pwrite(fd, buf, NAPOT_BYTES, 0);
	free(buf);
	if (ret != NAPOT_BYTES)
		goto fail;
	return fd;

fail:
	close(fd);
	return -1;
}

static int test_file_mappings(int probe_fd)
{
	struct svnapot_probe_query query;
	struct aligned_map map = { .raw = MAP_FAILED };
	unsigned char byte;
	size_t i;
	int fd;

	TEST_LOG("shared and private file mappings fold");
	fd = create_backing_file();
	if (fd < 0)
		TEST_FAIL("create backing file: %s", strerror(errno));

	if (map_file(&map, fd, PROT_READ | PROT_WRITE, MAP_SHARED))
		goto fail;
	for (i = 0; i < SVNAPOT_PROBE_PTES; i++)
		ASSERT(map.addr[i * PTE_BYTES] == (unsigned char)(0x40 + i),
		       "shared file slot %zu mismatch", i);
	if (query_ptes(probe_fd, map.addr, &query) || check_folded(&query))
		goto fail;
	fill_slots(map.addr, 0x50);
	if (msync(map.addr, NAPOT_BYTES, MS_SYNC))
		TEST_FAIL("shared msync: %s", strerror(errno));
	release_aligned(&map);

	for (i = 0; i < SVNAPOT_PROBE_PTES; i++) {
		if (pread(fd, &byte, 1, i * PTE_BYTES) != 1)
			TEST_FAIL("pread slot %zu: %s", i, strerror(errno));
		ASSERT(byte == (unsigned char)(0x50 + i),
		       "file slot %zu got 0x%x", i, byte);
	}

	map.raw = MAP_FAILED;
	if (map_file(&map, fd, PROT_READ, MAP_PRIVATE))
		goto fail;
	for (i = 0; i < SVNAPOT_PROBE_PTES; i++)
		ASSERT(map.addr[i * PTE_BYTES] == (unsigned char)(0x50 + i),
		       "private file slot %zu mismatch", i);
	if (query_ptes(probe_fd, map.addr, &query) || check_folded(&query))
		goto fail;
	if (mprotect(map.addr + 3 * PTE_BYTES, PTE_BYTES,
		     PROT_READ | PROT_WRITE))
		TEST_FAIL("private mprotect: %s", strerror(errno));
	map.addr[3 * PTE_BYTES] = 0x7a;
	if (query_ptes(probe_fd, map.addr, &query) ||
	    check_group_atomic(&query))
		goto fail;
	if (pread(fd, &byte, 1, 3 * PTE_BYTES) != 1)
		TEST_FAIL("private pread: %s", strerror(errno));
	ASSERT(byte == 0x53, "private COW changed file: 0x%x", byte);

	release_aligned(&map);
	close(fd);
	return 0;

fail:
	release_aligned(&map);
	close(fd);
	return 1;
}

static int test_fork_cow(int fd)
{
	struct svnapot_probe_query query;
	struct aligned_map map = { .raw = MAP_FAILED };
	pid_t pid;
	int status;

	TEST_LOG("fork and sub-PTE COW preserve group consistency");
	if (map_anon(&map, PROT_READ | PROT_WRITE))
		return 1;
	fill_slots(map.addr, 0x60);
	if (query_ptes(fd, map.addr, &query) || check_folded(&query))
		goto fail;

	pid = fork();
	if (pid < 0)
		TEST_FAIL("fork: %s", strerror(errno));
	if (pid == 0) {
		map.addr[5 * PTE_BYTES] = 0x7b;
		if (query_ptes(fd, map.addr, &query) ||
		    check_group_atomic(&query))
			_exit(2);
		if (map.addr[4 * PTE_BYTES] != 0x64 ||
		    map.addr[6 * PTE_BYTES] != 0x66)
			_exit(3);
		_exit(0);
	}

	if (waitpid(pid, &status, 0) != pid)
		TEST_FAIL("waitpid: %s", strerror(errno));
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	       "child COW status 0x%x", status);
	if (verify_slots(map.addr, 0x60))
		goto fail;
	if (query_ptes(fd, map.addr, &query) || check_group_atomic(&query))
		goto fail;

	release_aligned(&map);
	return 0;

fail:
	release_aligned(&map);
	return 1;
}

static int test_partial_munmap(int fd)
{
	struct svnapot_probe_query query;
	struct aligned_map map = { .raw = MAP_FAILED };

	TEST_LOG("partial munmap tears down a folded group safely");
	if (map_anon(&map, PROT_READ | PROT_WRITE))
		return 1;
	fill_slots(map.addr, 0x20);
	if (query_ptes(fd, map.addr, &query) || check_folded(&query))
		goto fail;
	if (munmap(map.addr + 8 * PTE_BYTES, PTE_BYTES))
		TEST_FAIL("partial munmap: %s", strerror(errno));
	ASSERT(map.addr[7 * PTE_BYTES] == 0x27, "left munmap neighbor corrupt");
	ASSERT(map.addr[9 * PTE_BYTES] == 0x29, "right munmap neighbor corrupt");
	if (query_ptes(fd, map.addr, &query))
		goto fail;
	ASSERT(query.napot_mask == 0, "partial munmap left NAPOT mask 0x%x",
	       query.napot_mask);
	release_aligned(&map);
	return 0;

fail:
	release_aligned(&map);
	return 1;
}

static void on_alarm(int sig)
{
	(void)sig;
	_exit(124);
}

int main(void)
{
	struct svnapot_probe_query query;
	struct aligned_map map = { .raw = MAP_FAILED };
	int fd;

	signal(SIGALRM, on_alarm);
	alarm(120);

	fd = open("/dev/svnapot_probe", O_RDWR);
	if (fd < 0) {
		TEST_LOG("SKIP: /dev/svnapot_probe is not loaded");
		return 0;
	}

	if (map_anon(&map, PROT_READ | PROT_WRITE))
		return 1;
	map.addr[0] = 1;
	if (query_ptes(fd, map.addr, &query))
		return 1;
	release_aligned(&map);
	ASSERT(query.pte_shift == 12, "PTE_SHIFT=%u", query.pte_shift);
	ASSERT(query.pg_shift == 16, "PG_SHIFT=%u", query.pg_shift);

	if (test_anon_and_mprotect(fd) ||
	    test_zero_and_unaligned(fd) ||
	    test_file_mappings(fd) ||
	    test_fork_cow(fd) ||
	    test_partial_munmap(fd)) {
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
