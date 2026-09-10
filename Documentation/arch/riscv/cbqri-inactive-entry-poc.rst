.. SPDX-License-Identifier: GPL-2.0

=========================================
CBQRI bandwidth inactive-entry PoC report
=========================================

Purpose
-------

This document records a functional proof of concept for the proposed CBQRI
bandwidth inactive-entry extension.  The extension defines the following
unique allocation as inactive::

  useShared = 0
  Rbwb      = 0
  Mweight   = 0

The ``sharedAT`` field has no effect when ``useShared`` is zero.  The PoC
therefore neither requires nor verifies a particular ``sharedAT`` value for
an inactive entry.

The PoC validates the controller and operating-system control plane.  It does
not validate bandwidth performance and does not define an arbitration
algorithm.  The discovery value ``bc_capabilities.VER=0x11`` is provisional
and remains subject to ARC confirmation.

Source revisions
----------------

The recorded validation used these immutable revisions:

* Linux implementation: ``e1475c3393857fb8b14be827bacad2ba64deb2bf``
* QEMU implementation and tests:
  ``6fda7b6269e39f53d2d0f60b925bf1a8c1ad7189``
* QEMU upstream base:
  ``1df256f5968e9f7c3c4533a1383b071c044a36d6``
* Proposal:
  ``9f518cd1b63c850678c4f4e3ea9425db4ccb5fca``

The QEMU branch replays the latest clean CBQRI and RQSC controller work from
Drew Fustini's public development branch on the upstream base above.  It does
not include monitoring simulation, Device Tree experiments, local scripts, or
WIP/NFU commits.  This test instantiates only a bandwidth controller, so the
experimental capacity-controller PPTT topology patches are not required.

The source branches are:

* https://github.com/cuiyunhui/qemu/tree/cbqri-inactive-entry-poc
* https://github.com/cuiyunhui/linux/tree/cbqri-inactive-entry-poc

Test environment
----------------

The test was run on an AArch64 host with:

* QEMU system emulation for ``riscv64-softmmu``;
* OpenSBI 1.8.1 from the QEMU source tree;
* EDK2 firmware;
* a RISC-V initramfs containing ``mount`` and ``devmem``; and
* GCC 15.2.1 and binutils 2.46 for the Linux build.

The Linux configuration enabled at least::

  CONFIG_RISCV_ISA_SSQOSID=y
  CONFIG_ACPI=y
  CONFIG_ARCH_HAS_CPU_RESCTRL=y
  CONFIG_PROC_CPU_RESCTRL=y
  CONFIG_RESCTRL_FS=y
  CONFIG_DEVMEM=y
  CONFIG_STRICT_DEVMEM=y
  CONFIG_IO_STRICT_DEVMEM=y

QEMU register-level validation
------------------------------

Build QEMU and the CBQRI qtest::

  ./configure \
    --python=/path/to/python3.12 \
    --target-list=riscv64-softmmu \
    --disable-docs \
    --disable-werror
  ninja -C build qemu-system-riscv64 tests/qtest/riscv-cbqri-test

Run the test::

  cd build
  QTEST_QEMU_BINARY=./qemu-system-riscv64 \
    tests/qtest/riscv-cbqri-test --tap -k

Expected result::

  1..4
  ok 1 /riscv64/riscv/cbqri/legacy-rejects-zero-rbwb
  ok 2 /riscv64/riscv/cbqri/inactive-entry
  ok 3 /riscv64/riscv/cbqri/access-types
  ok 4 /riscv64/riscv/cbqri/code-only-access-type

The tests cover:

* CBQRI 1.0 rejection of a zero-``Rbwb`` unique allocation;
* discovery of the provisional inactive-entry version;
* active-to-inactive and inactive-to-active transitions;
* release and reallocation of the controller-wide reserved budget;
* rejection of ``Rbwb=0, Mweight>0``;
* an inactive entry with a non-zero ignored ``sharedAT`` field;
* independent DATA and CODE entries; and
* a controller supporting CODE but not DATA.

Linux end-to-end validation
---------------------------

QEMU configuration
------------------

The bandwidth controller was instantiated with::

  -device riscv.cbqri.bandwidth,\
  max_mcids=256,max_rcids=8,nbwblks=1024,mrbwb=819,\
  inactive-entry=on,\
  mon_op_config_event=off,mon_op_read_counter=off,\
  mon_evt_id_none=off,mon_evt_id_rdwr_count=off,\
  mon_evt_id_rdonly_count=off,mon_evt_id_wronly_count=off,\
  mmio_base=0x04828000

The kernel command line included ``iomem=relaxed`` so that ``devmem`` could be
used to inspect the emulated controller during validation.

Discovery
~~~~~~~~~

The guest reported::

  RQSC controllers: total=1 cache=0 bw=1
  CBQRI controllers: total=1 cache=0 bw=1
  exposed: alloc=1 mon=0 cdp_l2=0 cdp_l3=0

The capability register at ``0x04828000`` returned::

  0x0000033300040011

The low byte is the provisional version ``0x11``.  ``NBWBLKS`` is 1024 and
``MRBWB`` is 819.

Initial inactive state
~~~~~~~~~~~~~~~~~~~~~~

The resctrl filesystem was mounted and reported eight control IDs::

  mkdir -p /sys/fs/resctrl
  mount -t resctrl none /sys/fs/resctrl
  cat /sys/fs/resctrl/info/MB/num_closids

The root group started active::

  MWEIGHT:0=255
       MB:0=80

After reducing the root reservation and creating a child group::

  echo 'MB:0=40' > /sys/fs/resctrl/schemata
  mkdir /sys/fs/resctrl/group_a
  cat /sys/fs/resctrl/group_a/schemata

the new group started inactive::

  MWEIGHT:0=0
       MB:0=0

Activation and readback
~~~~~~~~~~~~~~~~~~~~~~~

The group was activated with::

  echo 'MB:0=20' > /sys/fs/resctrl/group_a/schemata
  echo 'MWEIGHT:0=7' > /sys/fs/resctrl/group_a/schemata

``READ_LIMIT`` for RCID 1 and DATA returned::

  0x00000000007000CC

This corresponds to ``Mweight=7`` and ``Rbwb=204``.  A separate CODE entry was
configured as ``Mweight=3, Rbwb=50`` and read back as::

  0x0000000000300032

Retirement and reuse
~~~~~~~~~~~~~~~~~~~~

After removing the group::

  rmdir /sys/fs/resctrl/group_a

``READ_LIMIT`` returned zero for both RCID 1 entries::

  DATA: 0x0000000000000000
  CODE: 0x0000000000000000

A new group reused RCID 1.  Writing the same ``MB=20`` value used by the old
group produced ``Rbwb=0xCC`` again.  This verifies that hardware state and the
Linux control-value cache were both updated during retirement.

Repeated lifecycle test
~~~~~~~~~~~~~~~~~~~~~~~

The following sequence completed 20 times::

  i=1
  while [ $i -le 20 ]; do
      mkdir /sys/fs/resctrl/cycle || break
      echo 'MB:0=20' > /sys/fs/resctrl/cycle/schemata || break
      rmdir /sys/fs/resctrl/cycle || break
      i=$((i + 1))
  done
  echo CYCLES_COMPLETED=$((i - 1))

The result was::

  CYCLES_COMPLETED=20

The final DATA and CODE entries were both zero.  The kernel log contained no
``failed with status``, ``update failed``, or ``inactive readback failed``
messages.

Recorded result
---------------

The following checks passed:

* QEMU build on the recorded upstream base;
* QEMU register-level tests: 4/4;
* Linux RISC-V Image build with GCC 15.2.1;
* ARM64 MPAM/resctrl build of the common release hook;
* RQSC and CBQRI discovery;
* inactive group creation;
* DATA and CODE cleanup;
* same-value RCID reuse; and
* 20 repeated create/configure/remove/reuse cycles.

Artifact hashes from the recorded run::

  1c218650f4b545e30360fe62728b878850e16b2f0083d24be0d7335491cbcb70  qemu-system-riscv64
  e15f5c4861005e90087fd716bbbdb04a027482e89ef01b379bc205a06379e638  Linux Image
  894e2aef99590fc07ec6c60ab00282b8bc5d5d5bb2a1d0c6ada0c52df24274c0  OpenSBI firmware
  ba4f670d3fb251a247b664199ebf35ae55c1e80de7f2e3db15a44a93a5c221fa  initramfs
  c3b779f86671c0cadf9cbdb274917a20210aeceb6f58b42d68406cdddfa48869  EDK2 firmware

Limitations
-----------

The PoC does not implement or validate a cycle-accurate bandwidth arbiter and
makes no performance claim.  QEMU fault injection for a failed cleanup has not
been exercised end to end; the Linux error path keeps the control ID allocated
when architecture cleanup reports an error.
