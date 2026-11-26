.. SPDX-License-Identifier: GPL-2.0-only

==================================
CBQRI sparse identifier mapping PoC
==================================

This PoC validates a draft CBQRI follow-on extension that separates the
12-bit request RCID and MCID namespaces from controller-local allocation
entries and monitoring counters.

Discovery
=========

The provisional mapping capability version is 0x12. RCID_MAP and MCID_MAP
advertise allocation mapping and direct monitoring mapping. RQSC counts still
describe physical allocation entries and counters. resctrl.logical_ids=1 opts
into the larger request namespace; default boot retains the legacy limits.

Only direct MCID monitoring is implemented. RCID-prefixed monitoring is not
advertised or configured. Legacy controllers and mixed components retain safe
physical identifier limits.

Allocation behavior
===================

Creating a mapped group allocates a logical CLOSID without reserving a physical
entry. Unconfigured policies use entry 0 and follow the current Root policy.
Initialization programs valid Root policies into hardware.

The first explicit policy write, including a same-value write, reserves an
unbound physical entry in each affected controller, copies the Root policy,
programs staged controls using physical RCIDs, and publishes the bindings::

  preflight -> reserve -> program -> publish

An existing binding is updated in place without a spare shadow entry. Its
mapping stays unchanged. There is no global atomic policy update across
controllers. Physical exhaustion returns ENOSPC and leaves an unbound RCID
using entry 0. Failed publication rolls back new tags where possible;
unrecoverable entries remain quarantined.

Release removes tags before freeing physical entries. Reset skips unmapped
logical IDs. Legacy bandwidth teardown uses inactive entries when supported,
or the smallest valid reservation otherwise.

Mapping recovery
================

Initialization scans physical entries with READ_ENTRY (OP=3), which returns
the logical RCID tag and VALID bit. Stale bindings are removed before entries
enter the free pool. Unreadable or unremovable entries remain quarantined.

Monitoring behavior
===================

CONFIG_EVENT activates monitoring when Linux resets/enables a group's event,
before user reads. Preflight covers every controller in the monitor domain.
Reads never allocate counters. Unbound MCIDs report No_counter, exposed as
Unassigned.

CONFIG_EVENT with EVT_ID=None stops monitoring and removes the binding. A
failed stop retains the logical RMID on the generic pending-release list.
Later RMID allocations retry release. Partial release can free one physical
counter while the logical RMID remains unavailable because another controller
has not completed release.

Successful stop must guarantee safe reuse, including isolation from events
belonging to the old monitoring lifetime. The deterministic QEMU counters do
not simulate old cache-line events and cannot prove that hardware guarantee.
Legacy capacity monitoring retains occupancy-based reuse protection.

Controllers for one logical component share a domain. Allocation updates
program every controller; monitoring sums their matching counters.

Validated configuration
=======================

Nine qtests cover legacy allocation, inactive entries, access types, sparse
RCID/MCID mapping, READ_ENTRY and failed counter release. The Linux test uses
two bandwidth controllers with four physical entries and two counters each.
Their deterministic values 17 and 19 make the expected aggregate 36.

Linux end-to-end checks cover:

* 4096 logical CLOSIDs and creation without physical allocation;
* explicit policy writes, local exhaustion and last_cmd_status;
* existing-entry updates with a full physical table;
* synchronized configuration, release and reuse;
* activation before reads, counter exhaustion and Unassigned reads;
* explicit monitor activation after counter release;
* failed stop, retained RMID, partial release and successful retry;
* old MCIDs returning No_counter after successful stop; and
* unmount/reset with unmapped logical identifiers.

The generic RFC was also validated on arm64 MPAM and AMD x86. Those tests
validate their architecture adaptations, rather than future CBQRI registers.

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

The expected result is ``1..9`` followed by nine ``ok`` lines.

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

  IDMAP_V3_E2E_PASS:logical_ids_activation_checked_release_reuse

The script validates capability discovery, logical ID counts, default root
inheritance, multiple control updates, multi-controller programming, RCID
exhaustion and reuse, ``last_cmd_status``, MCID exhaustion and reuse, and
counter summation. Add --release-failures 2 to exercise failed stop
and the generic pending-release retry path.

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
