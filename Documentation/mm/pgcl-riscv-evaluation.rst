.. SPDX-License-Identifier: GPL-2.0

===========================================
RISC-V PGCL and 64 KiB mTHP evaluation
===========================================

This report records a proof-of-concept evaluation of the ``PG_SIZE`` /
``PTE_SIZE`` split on a RISC-V server.  It is based on the
`pte_size branch
<https://git.kernel.org/pub/scm/linux/kernel/git/kas/linux.git/log/?h=pte_size>`_
and the RISC-V changes in the
`pgcl_riscv branch
<https://github.com/cuiyunhui/linux/commits/pgcl_riscv>`_.

Three configurations were compared:

.. list-table::
   :header-rows: 1

   * - ID
     - Configuration
     - Allocator page
     - PTE
     - Userspace ABI
     - 64 KiB translation
   * - A
     - Conventional 4 KiB kernel
     - 4 KiB
     - 4 KiB
     - 4 KiB
     - None
   * - B
     - 64 KiB ``PG_SIZE`` / 4 KiB ``PTE_SIZE`` (PGCL)
     - 64 KiB
     - 4 KiB
     - 4 KiB
     - RISC-V Svnapot PTE folding
   * - C
     - 4 KiB base pages with 64 KiB anonymous mTHP
     - 4 KiB
     - 4 KiB
     - 4 KiB
     - The same RISC-V Svnapot PTE folding

Both B and C use the same PTE-folding implementation.  Their main
difference is whether 64 KiB is the allocator-page granularity or a
best-effort anonymous mTHP size.

Test controls
=============

The tests used the same server, root filesystem, userspace binaries and
datasets.  Single-threaded memory and direct-I/O tests were pinned to CPU
30.  The 16-CPU UnixBench tests were restricted to CPUs 8-23.  No cpufreq
driver was active during the measurements.

All configurations reported a 4 KiB userspace page size.  A and B had all
THP sizes disabled.  C had global THP disabled and only 64 KiB anonymous
mTHP set to ``always``.

Unless otherwise noted, application results are medians rather than the
best result.  Coefficient of variation (CV) is reported where the raw
multi-run data is available.  Kernel taint and the kernel log were checked
before and after the formal runs.

PTE-folding isolation
=====================

The isolated comparison used the same PGCL source and configuration.  The
functional switch was ``CONFIG_RISCV_ISA_SVNAPOT``.  ``lat_mem_rd`` used a
256 MiB working set and 4 KiB pointer stride, SPEC CPU2017 ``505.mcf_r``
used the reference input, and Redis used a fixed three-million-request GET
workload.

.. list-table::
   :header-rows: 1

   * - Workload
     - Metric
     - Folding off
     - Folding on
     - Change
     - Off/On CV
   * - ``lat_mem_rd``
     - Latency
     - 35.633 ns
     - 30.145 ns
     - 15.40% faster
     - 0.04% / 0.07%
   * - ``lat_mem_rd``
     - DTLB miss/access
     - 7.202%
     - 0.910%
     - 87.37% lower
     - -
   * - SPEC ``505.mcf_r``
     - Elapsed time
     - 282.68 s
     - 273.61 s
     - 3.21% faster
     - 0.04% / 0.06%
   * - SPEC ``505.mcf_r``
     - DTLB miss/access
     - 6.062%
     - 1.021%
     - 83.15% lower
     - -
   * - Redis GET
     - Throughput
     - 111,205.84 RPS
     - 110,885.23 RPS
     - 0.29% lower
     - 0.49% / 0.64%
   * - Redis GET
     - DTLB miss/access
     - 4.901%
     - 4.210%
     - 14.10% lower
     - -

``lat_mem_rd`` and ``505.mcf_r`` demonstrate a measurable benefit when
the workload is DTLB-sensitive.  Redis demonstrates the boundary of that
result: its DTLB ratio decreased, but the throughput change was smaller
than run-to-run variation.

The raw-PTE probe observed 16 of 16 eligible groups folded with Svnapot
enabled and zero folded groups in a forced-split mapping.  Every SPEC
output matched the valid reference byte-for-byte.

A/B/C TLB-sensitive workloads
=============================

Lower latency, elapsed time and DTLB miss/access are better.

.. list-table::
   :header-rows: 1

   * - Workload
     - Metric
     - A
     - B
     - C
     - B vs A
     - C vs A
   * - ``lat_mem_rd``
     - Latency
     - 32.838 ns
     - 30.139 ns
     - 29.049 ns
     - 8.22% faster
     - 11.54% faster
   * - ``lat_mem_rd``
     - DTLB miss/access
     - 5.6768%
     - 0.9180%
     - 0.9200%
     - 83.83% lower
     - 83.79% lower
   * - SPEC ``505.mcf_r``
     - Elapsed time
     - 282.000 s
     - 273.975 s
     - 273.781 s
     - 2.85% faster
     - 2.91% faster
   * - SPEC ``505.mcf_r``
     - DTLB miss/access
     - 6.0635%
     - 1.0189%
     - 1.0234%
     - 83.20% lower
     - 83.12% lower

B and C produced nearly identical DTLB miss reductions in these
workloads.

Application benchmarks
======================

The values are medians.  Percentages are relative to A.

.. list-table::
   :header-rows: 1

   * - Workload
     - Unit
     - A median/CV
     - B median/CV
     - C median/CV
     - B vs A
     - C vs A
   * - Redis GET
     - RPS
     - 112,278.8 / 1.23%
     - 109,577.0 / 0.13%
     - 111,699.4 / 0.33%
     - -2.41%
     - -0.52%
   * - MariaDB point select
     - QPS
     - 325,552.6 / 0.37%
     - 336,338.0 / 0.20%
     - 319,938.6 / 0.40%
     - +3.31%
     - -1.72%
   * - Nginx 1 KiB
     - RPS
     - 671,841.8 / 0.26%
     - 685,126.6 / 0.24%
     - 699,447.4 / 0.32%
     - +1.98%
     - +4.11%
   * - Nginx 64 KiB
     - RPS
     - 337,416.8 / 0.46%
     - 399,272.1 / 0.08%
     - 339,765.6 / 0.13%
     - +18.33%
     - +0.70%
   * - Nginx 1 MiB
     - RPS
     - 28,609.2 / 0.15%
     - 34,120.6 / 0.24%
     - 28,800.5 / 0.14%
     - +19.26%
     - +0.67%

The Nginx test used a warmed page cache, ``sendfile`` and loopback TCP.
PGCL showed a clear advantage for large responses, while anonymous
64 KiB mTHP remained close to A.  This workload includes page-cache,
``sendfile`` and TCP processing that anonymous mTHP does not change.

A separate physical-NIC iPerf comparison showed a similar PGCL/mTHP
difference with one unidirectional stream, while the difference became
small with multiple concurrent streams.  A possible contributor is
``page_pool`` allocation geometry: an order-0 allocation on PGCL returns
one 64 KiB allocator page, while a 4 KiB kernel needs an explicit order-4
allocation to obtain the same contiguous size.  This is an allocator and
buffer-backing property, not Svnapot folding of the kernel linear map.

UnixBench
=========

The complete suite used the same UnixBench binaries for all
configurations.  The table below contains aggregate throughput from the
16-CPU concurrent run, not per-CPU throughput.  The final row is the
normalized UnixBench system index.

.. list-table::
   :header-rows: 1

   * - Subtest
     - Unit
     - A
     - B
     - C
     - B vs A
     - C vs A
   * - Dhrystone 2
     - lps
     - 685,268,041.3
     - 685,179,773.5
     - 685,245,389.5
     - -0.01%
     - -0.00%
   * - Double-Precision Whetstone
     - MWIPS
     - 111,356.6
     - 111,391.6
     - 111,401.6
     - +0.03%
     - +0.04%
   * - Execl
     - lps
     - 57,735.3
     - 22,005.1
     - 56,802.8
     - -61.89%
     - -1.62%
   * - File Copy 1024
     - KBps
     - 11,399,445
     - 11,615,862
     - 12,356,876
     - +1.90%
     - +8.40%
   * - File Copy 256
     - KBps
     - 4,708,784
     - 3,791,580
     - 4,723,147
     - -19.48%
     - +0.31%
   * - File Copy 4096
     - KBps
     - 16,077,818
     - 17,738,753
     - 16,591,056
     - +10.33%
     - +3.19%
   * - Pipe Throughput
     - lps
     - 29,478,186.4
     - 29,732,396.2
     - 29,420,154.6
     - +0.86%
     - -0.20%
   * - Pipe Context Switching
     - lps
     - 4,557,300.2
     - 4,594,567.6
     - 4,114,045.1
     - +0.82%
     - -9.73%
   * - Process Creation
     - lps
     - 57,085.4
     - 17,428.0
     - 55,532.7
     - -69.47%
     - -2.72%
   * - Shell Scripts (1)
     - lpm
     - 114,394.2
     - 50,205.1
     - 113,010.8
     - -56.11%
     - -1.21%
   * - Shell Scripts (8)
     - lpm
     - 16,449.7
     - 6,316.9
     - 16,300.2
     - -61.60%
     - -0.91%
   * - System Call Overhead
     - lps
     - 21,950,551.6
     - 22,123,042.0
     - 22,103,177.2
     - +0.79%
     - +0.70%
   * - System Benchmarks Index
     - index
     - 20,265.5
     - 14,513.1
     - 20,186.3
     - -28.39%
     - -0.39%

The PGCL loss is concentrated in Execl, Process Creation and Shell.
Arithmetic, pipe throughput and ordinary system-call throughput remain
close to A.

File fault-around investigation
-------------------------------

The original PGCL code disabled file-backed fault-around when
``PTES_PER_PAGE > 1`` as a correctness workaround for sub-PG mappings.
The final branch restores PTE-granular file fault-around with mapping and
file-size bounds.

The calls below are normalized per completed exec:

.. list-table::
   :header-rows: 1

   * - Function
     - Original B
     - Final PGCL
     - A
   * - ``filemap_map_pages()``
     - 0
     - 21.28
     - 21.66
   * - ``filemap_fault()``
     - 123.03
     - 4.31
     - 3.10

The single-CPU full suite improved after the change:

.. list-table::
   :header-rows: 1

   * - Metric
     - Original B
     - Final PGCL
     - Change
   * - System Benchmarks Index
     - 1,445.4
     - 1,479.8
     - +2.38%
   * - Execl
     - 3,317.5 lps
     - 3,865.6 lps
     - +16.52%

The same change did not recover the 16-CPU suite:

.. list-table::
   :header-rows: 1

   * - Configuration
     - System index
     - Execl
     - Difference from A
   * - A
     - 20,265.5
     - 57,735.3 lps
     - baseline
   * - Original B
     - 14,513.1
     - 22,005.1 lps
     - -28.39%
   * - Final PGCL, run 1
     - 14,588.2
     - 21,260.3 lps
     - -28.01%
   * - Final PGCL, independent repeat
     - 14,394.2
     - 21,240.9 lps
     - -28.97%
   * - C
     - 20,186.3
     - 56,802.8 lps
     - -0.39%

File fault-around was a real single-CPU issue, but it is not the primary
remaining cause of the 16-CPU regression.  The concurrent Execl, Process
Creation and Shell paths have another bottleneck.

Direct-I/O
==========

The final direct-read comparison used fio 3.39, ``psync``, ``direct=1``,
``iodepth=1``, ``numjobs=1``, a 16 GiB file on ext4/NVMe, CPU 30, a
120-second runtime and a 5-second ramp.  Each result has three measured
runs.  Bandwidth is in MiB/s and higher is better.

.. list-table::
   :header-rows: 1

   * - Workload
     - A median/CV
     - B median/CV
     - C median/CV
     - B vs A
     - C vs A
   * - 4 KiB random read
     - 69.088 / 0.061%
     - 69.042 / 0.026%
     - 69.089 / 0.090%
     - -0.07%
     - +0.00%
   * - 64 KiB random read
     - 549.718 / 0.005%
     - 541.853 / 0.008%
     - 542.942 / 0.030%
     - -1.43%
     - -1.23%
   * - 4 KiB sequential read
     - 494.633 / 1.654%
     - 496.513 / 0.303%
     - 479.307 / 0.394%
     - +0.38%
     - -3.10%
   * - 64 KiB sequential read
     - 3,279.804 / 0.102%
     - 3,036.649 / 0.150%
     - 3,049.891 / 0.219%
     - -7.41%
     - -7.01%

User-page pinning investigation
-------------------------------

An A/C kprobe comparison over 20,000 64 KiB I/Os produced:

.. list-table::
   :header-rows: 1

   * - Function
     - A
     - C
   * - ``bio_iov_iter_get_pages()``
     - 20,000
     - 20,000
   * - ``pin_user_pages_fast()``
     - 20,000
     - 0
   * - ``pin_user_pte_page()``
     - 0
     - 320,000
   * - ``gup_vma_lookup()``
     - 0
     - 320,031

A performs one batched fast-GUP operation per 64 KiB I/O.  The tested C
code performs sixteen PTE-granular pins and VMA lookups to preserve the
4 KiB sub-page offsets.  B and C show similar 64 KiB sequential-read
losses because the tested code shares this PTE-granular extraction path.

This is an implementation overhead rather than a fundamental limit of
PGCL or mTHP.  Potential optimizations include retaining batched fast-GUP
when ``PG_SIZE == PTE_SIZE`` and providing batched PTE-granular pinning
that preserves sub-page offsets for split-PG configurations.
