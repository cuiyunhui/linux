.. SPDX-License-Identifier: GPL-2.0-only

========================================
Supervisor-mode QoS association PoC
========================================

This PoC evaluates a possible Ssqosid follow-on extension that provides a
separate QoS association for supervisor-mode execution. It is independent of
the CBQRI RCID/MCID mapping PoC.

Architectural model
===================

The experimental ``Ssqosassoc`` extension adds one per-hart
``srmcfg_assoc`` CSR. The temporary CSR address is ``0x182``. Its low 32 bits
contain::

  11:0    RCID
  12      RCID_EN
  27:16   MCID
  28      MCID_EN
  31      ASSOC_EN

All enable bits reset to zero. When ``ASSOC_EN`` is set during S-mode
execution with ``V=0``, ``RCID_EN`` and ``MCID_EN`` independently select the
corresponding identifier from ``srmcfg_assoc``. Other identifiers continue to
come from ``srmcfg``. U-mode, VS-mode, VU-mode, and M-mode use ``srmcfg``.

Linux interface
===============

When ``ssqosassoc`` is detected, Linux exposes these files under the resctrl
``info`` directory::

  kernel_mode
  kernel_mode_assignment

The default mode is ``inherit_ctrl_and_mon``, which leaves ``ASSOC_EN``
clear. The two global modes are::

  global_assign_ctrl_inherit_mon
  global_assign_ctrl_assign_mon

``kernel_mode_assignment`` selects an existing control or monitoring group
using ``CTRL_MON/MON/`` syntax. The association is enabled only after both a
global mode and a group have been selected. Writing ``none`` clears the
assignment. Selecting the inherit mode, deleting the assigned group, or
unmounting resctrl disables the association on every online hart. CPU hotplug
restores the current association on a newly online hart.

Usage example
-------------

The following assigns RCID and MCID from the ``kernel`` group::

  mount -t resctrl -o debug resctrl /sys/fs/resctrl
  mkdir /sys/fs/resctrl/kernel
  echo global_assign_ctrl_assign_mon > /sys/fs/resctrl/info/kernel_mode
  echo kernel// > /sys/fs/resctrl/info/kernel_mode_assignment

To disable the association::

  echo inherit_ctrl_and_mon > /sys/fs/resctrl/info/kernel_mode

Validation
==========

``tools/testing/selftests/resctrl/ssqosassoc_qemu.py`` boots a matching QEMU
PoC and verifies the default state, mode and group configuration, RCID-only
mode, disable, re-enable, and automatic disable when the assigned group is
removed. The QEMU trace confirms the CSR values on both harts.

Build the matching QEMU PoC in a separate build directory::

  mkdir qemu-build
  cd qemu-build
  ../qemu/configure --target-list=riscv64-softmmu
  ninja qemu-system-riscv64

Build the Linux image with a RISC-V cross compiler::

  make ARCH=riscv CROSS_COMPILE=riscv64-unknown-linux-gnu- olddefconfig
  make ARCH=riscv CROSS_COMPILE=riscv64-unknown-linux-gnu- -j$(nproc) Image

Run the end-to-end test::

  python3 tools/testing/selftests/resctrl/ssqosassoc_qemu.py \
    --qemu /path/to/qemu-system-riscv64 \
    --kernel arch/riscv/boot/Image \
    --initrd /path/to/rootfs.cpio \
    --firmware /path/to/RISCV_VIRT_CODE.fd \
    --opensbi /path/to/fw_dynamic.bin

Run the compatibility test without exposing ``Ssqosassoc``::

  python3 tools/testing/selftests/resctrl/ssqosassoc_qemu.py \
    --legacy-hardware \
    --qemu /path/to/qemu-system-riscv64 \
    --kernel arch/riscv/boot/Image \
    --initrd /path/to/rootfs.cpio \
    --firmware /path/to/RISCV_VIRT_CODE.fd \
    --opensbi /path/to/fw_dynamic.bin

To test an older kernel on hardware that exposes ``Ssqosassoc``, provide the
older image and replace ``--legacy-hardware`` with ``--legacy-software``.

PoC boundaries
==============

The extension name, CSR address, and field layout are provisional. The QEMU
CBQRI model does not yet route CPU memory requests through a simulated cache
or bandwidth controller, so the PoC validates CSR state and effective-ID
selection rather than cache occupancy or bandwidth enforcement.

The current OpenSBI test firmware does not enable ``mstateen0.SRMCFG``. Run
the PoC with a CPU configuration that does not enable ``Smstateen``, or update
the firmware to grant S-mode access to the QoS association state.
