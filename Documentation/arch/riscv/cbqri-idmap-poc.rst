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

PoC boundaries
==============

The QEMU model validates the register state machine but does not generate real
cache or memory traffic. The provisional version and capability-bit encodings
require Architecture Review Committee assignment. A production Linux series
should split the common resctrl API preparation from the RISC-V driver changes
and add focused resctrl selftests.
