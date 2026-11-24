.. SPDX-License-Identifier: GPL-2.0-only

==================================
CBQRI sparse identifier mapping PoC
==================================

This PoC validates a draft CBQRI follow-on extension that separates the
12-bit request RCID and MCID namespaces from controller-local allocation
entries and monitoring counters.

Discovery
=========

An RCID mapping capable controller reports ``RCID_MAP`` in its capability
register. An MCID mapping capable controller reports ``MCID_MAP``. The RQSC
RCID and MCID counts continue to describe internal allocation entries and
physical counters. Linux exposes 4096 logical CLOSIDs or RMIDs when the
corresponding mapping capability is present.

Allocation behavior
===================

A newly created control group initially shares internal RCID 0 and consumes no
private allocation entry. An explicit schemata update reserves one unbound
internal RCID in every affected controller. Linux copies the current policy to
the reserved entries, programs every staged control using internal RCIDs, and
publishes the request-RCID tags only after all programming succeeds::

  preflight -> reserve -> program -> publish

An update to an existing private policy also uses a shadow entry. This keeps
the old mapping available for rollback until all controllers accept the new
configuration. If publication fails, Linux restores the old tags. Entries
whose state cannot be recovered are quarantined and are not silently reused.

Exhaustion is local to the affected controller and is reported through
``last_cmd_status``. Deleting a control group removes its tags and releases its
internal entries.

Mapping recovery
================

At initialization Linux issues ``READ_ENTRY`` for each nonzero internal RCID.
A recovered valid tag is explicitly removed before the entry joins the free
pool. An unreadable, duplicate, invalid, or unremovable entry remains busy and
quarantined. Recovery work is proportional to the physical entry count, not
the complete 4096-entry request namespace.

Monitoring behavior
===================

``CONFIG_EVENT`` binds a logical MCID to an available physical counter. An
unbound MCID returns ``No_counter``; Linux exposes this as ``Unassigned``.
``CONFIG_EVENT`` with ``EVT_ID=None`` removes the binding. Linux performs the
unbind when the logical RMID is actually released, after occupancy limbo when
that mechanism is active.

Controllers that describe the same logical component are grouped behind one
resctrl domain. Allocation updates program every physical controller in the
group. Monitoring reads select controllers that implement the requested event
and report the sum of their counters. A bandwidth-only platform uses its
memory proximity domain and CPU affinity to create a monitor-only L3 domain.

Validated configuration
=======================

The validation used QEMU ``virt`` with ACPI RQSC, an OCRA initramfs, and the
RISC-V Linux ``resctrl`` implementation. The controller capability version is
the provisional value ``0x12``.

The QEMU qtest suite covers legacy behavior, inactive bandwidth entries,
access types, sparse RCID mapping and sparse MCID mapping for both bandwidth
and capacity controllers. Eight tests pass.

The Linux end-to-end checks cover:

* 4096 logical CLOSIDs backed by four internal RCIDs per controller;
* group creation without allocating an internal RCID;
* transactional updates of reserved bandwidth and weight in one write;
* controller-local ``ENOSPC`` and generic ``last_cmd_status`` reporting;
* release and reuse of an internal RCID;
* no request-visible mapping after preflight failure;
* synchronized allocation on two physical bandwidth controllers;
* 4096 logical RMIDs backed by two physical counters per controller;
* MCID counter exhaustion, unbind, and reuse; and
* summing deterministic counters ``17 + 19`` to ``36`` for one logical domain.

Build and run
=============

The end-to-end test is intentionally not part of the default kselftest run. It
requires a matching QEMU PoC, EDK2, OpenSBI, and an initramfs containing a
shell, ``base64``, ``dd``, ``mount``, ``grep``, ``poweroff``, and
``/sbin/devmem``.

Build the QEMU PoC in a separate QEMU checkout::

  mkdir qemu-build
  cd qemu-build
  ../configure --target-list=riscv64-softmmu
  ninja qemu-system-riscv64 tests/qtest/riscv-cbqri-test

Run the QEMU register tests::

  QTEST_QEMU_BINARY=./qemu-system-riscv64 \
    ./tests/qtest/riscv-cbqri-test --tap -k

The expected result is ``1..8`` followed by eight ``ok`` lines.

Build the Linux PoC with a RISC-V cross compiler. The following ISA flags were
used for the validated build::

  export ARCH=riscv
  export CROSS_COMPILE=/path/to/riscv64-unknown-linux-gnu-
  export KCFLAGS='-march=rv64imafdc_zicbom_zicbop_zicboz_zicsr_zifencei_zihintpause_zawrs_zicond_zba_zbb_zbc_zbs_zkt_zacas_zabha -mabi=lp64'
  export KAFLAGS='-march=rv64imafdcv_zicbom_zicbop_zicboz_zicsr_zifencei_zihintpause_zawrs_zicond_zba_zbb_zbc_zbs_zkt_zacas_zabha -mabi=lp64'
  make olddefconfig
  make Image -j$(nproc)

The kernel configuration must enable ACPI and ``CONFIG_RISCV_ISA_SSQOSID``.
The latter selects the resctrl filesystem.

Run the end-to-end test from the Linux source tree::

  python3 tools/testing/selftests/resctrl/cbqri_idmap_qemu.py \
    --qemu /path/to/qemu-build/qemu-system-riscv64 \
    --kernel arch/riscv/boot/Image \
    --initrd /path/to/rootfs.cpio \
    --firmware /path/to/edk2-riscv-code.fd \
    --opensbi /path/to/opensbi-riscv64-generic-fw_dynamic.bin \
    --log /path/to/cbqri-idmap-e2e.log

The test succeeds only after observing::

  IDMAP_V3_E2E_PASS:logical_ids_multicontroller_sum_release_reuse

The script validates capability discovery, logical ID counts, default root
inheritance, a multi-control transaction, multi-controller programming, RCID
exhaustion and reuse, ``last_cmd_status``, MCID exhaustion and reuse, and
counter summation.

PoC boundaries
==============

The QEMU ``counter-value`` property supplies a deterministic value for E2E
topology testing; the model does not derive counters from real cache or memory
traffic. The current QEMU ``virt`` machine describes bandwidth resources but
does not provide PPTT cache nodes for capacity-controller RQSC entries, so the
automated Linux test uses two bandwidth controllers. Capacity mapping and
MCID register state machines remain covered by qtests.

The extension version and capability-bit encodings are provisional and require
Architecture Review Committee assignment. The Linux series keeps generic
resctrl transaction and release hooks separate from the RISC-V implementation.
