# Follow-up bugs uncovered during test authoring

Known issues that aren't covered by selftests yet, kept here so they don't
get lost.

## perf_event_open() ring buffer mmap fails with EINVAL on PG > PTE

`mmap()` of a perf ring buffer on 16K / 64K kernels returns `EINVAL`.
The check at `kernel/events/core.c:7447` requires `vma_size == PG_SIZE *
nr_pages`, but userspace computes the ring-buffer size from
`sysconf(_SC_PAGESIZE)` which returns `AT_PAGESZ == PTE_SIZE == 4K`.
Default `perf record` ring buffer is `(1 + 2^N) * 4K` which isn't a
PG_SIZE multiple on 16K/64K.

Reproducer:
```c
struct perf_event_attr attr = { .type = PERF_TYPE_SOFTWARE,
                                .config = PERF_COUNT_SW_CPU_CLOCK, };
int fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
/* 2 pages = 8K on a 4K-PAGESIZE userspace view */
mmap(NULL, 2 * sysconf(_SC_PAGESIZE), PROT_READ | PROT_WRITE,
     MAP_SHARED, fd, 0);
/* EINVAL on 16K kernel */
```

Fix direction: `perf_mmap()` should allow any multiple of the ring
buffer's natural page unit, not insist on PG_SIZE. Or userspace should
align to PG_SIZE — but that means teaching every perf-using tool about
the PTE/PG split.
