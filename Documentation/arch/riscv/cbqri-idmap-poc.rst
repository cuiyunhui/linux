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
private allocation entry. The first schemata write for a controller allocates
one nonzero internal RCID and programs its request-RCID tag. Exhaustion is
local to the affected controller and is reported through ``last_cmd_status``.
Deleting a control group removes all of its tags and releases the internal
entries.

Monitoring behavior
===================

``CONFIG_EVENT`` binds a logical MCID to an available physical counter. An
unbound MCID returns ``No_counter``; Linux exposes this as ``Unassigned``.
``CONFIG_EVENT`` with ``EVT_ID=None`` removes the binding. Linux performs the
unbind when the logical RMID is actually released, after occupancy limbo when
that mechanism is active.

Validated configuration
=======================

The validation used QEMU ``virt`` with ACPI RQSC, an OCRA initramfs, and the
RISC-V Linux ``resctrl`` implementation. The controller capability version is
the provisional value ``0x12``.

The QEMU qtest suite covers legacy behavior, inactive bandwidth entries,
access types, sparse RCID mapping and sparse MCID mapping for both bandwidth
and capacity controllers. Eight tests pass.

The Linux end-to-end checks cover:

* 4096 logical CLOSIDs backed by four internal RCIDs;
* group creation without allocating an internal RCID;
* controller-local ``ENOSPC`` and domain-specific ``last_cmd_status``;
* release and reuse of an internal RCID;
* no partial mapping after a multi-domain preflight failure;
* independent allocation on controllers with four and eight internal RCIDs;
* 4096 logical RMIDs backed by two physical counters; and
* MCID counter exhaustion, unbind, and reuse.

Build and run
=============

The end-to-end test is intentionally not part of the default kselftest run. It
requires a matching QEMU PoC, EDK2, OpenSBI, and an initramfs containing a
shell, ``base64``, ``mount``, ``grep``, ``poweroff``, and ``/sbin/devmem``.

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

  IDMAP_E2E_PASS:logical_ids_sparse_rcid_mcid_exhaust_release_reuse

The script validates the capability register, logical ID counts, default root
configuration, RCID exhaustion and reuse, ``last_cmd_status``, MCID counter
exhaustion, ``INV``, unbind, and counter reuse.

PoC boundaries
==============

The QEMU model validates the register state machine but does not generate real
cache or memory traffic. The provisional version and capability-bit encodings
require Architecture Review Committee assignment. A production Linux series
should split the common resctrl API preparation from the RISC-V driver changes
and add focused resctrl selftests.
