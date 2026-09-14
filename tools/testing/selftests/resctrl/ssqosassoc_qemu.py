#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the supervisor-mode QoS association QEMU/Linux PoC test."""

import argparse
import base64
import os
from pathlib import Path
import pty
import re
import select
import subprocess
import sys
import time

PASS_MARKER = "SSQOSASSOC_E2E_PASS"
LEGACY_PASS_MARKER = "SSQOSASSOC_LEGACY_PASS"

GUEST_TEST = r"""
fail() { echo SSQOSASSOC_E2E_FAIL:$1; poweroff -f; }
echo SSQOSASSOC_DEFAULT_BEGIN
mkdir -p /sys/fs/resctrl || fail mkdir_mountpoint
mount -t resctrl -o debug resctrl /sys/fs/resctrl || fail mount
grep -q '^\[inherit_ctrl_and_mon\]$' \
    /sys/fs/resctrl/info/kernel_mode || fail default_mode
[ "$(cat /sys/fs/resctrl/info/kernel_mode_assignment)" = "none" ] || \
    fail default_assignment
echo SSQOSASSOC_DEFAULT_END
mkdir /sys/fs/resctrl/kernel || fail mkdir_group
[ "$(cat /sys/fs/resctrl/kernel/ctrl_hw_id)" = "1" ] || fail closid
[ "$(cat /sys/fs/resctrl/kernel/mon_hw_id)" = "1" ] || fail rmid
echo global_assign_ctrl_assign_mon > /sys/fs/resctrl/info/kernel_mode || \
    fail set_ctrl_mon_mode
echo SSQOSASSOC_ASSIGN_BOTH_BEGIN
echo kernel// > /sys/fs/resctrl/info/kernel_mode_assignment || \
    fail assign_group
echo SSQOSASSOC_ASSIGN_BOTH_END
[ "$(cat /sys/fs/resctrl/info/kernel_mode_assignment)" = "kernel//" ] || \
    fail assignment_readback
echo SSQOSASSOC_CTRL_ONLY_BEGIN
echo global_assign_ctrl_inherit_mon > /sys/fs/resctrl/info/kernel_mode || \
    fail set_ctrl_mode
echo SSQOSASSOC_CTRL_ONLY_END
echo SSQOSASSOC_DISABLE_BEGIN
echo inherit_ctrl_and_mon > /sys/fs/resctrl/info/kernel_mode || \
    fail set_inherit_mode
echo SSQOSASSOC_DISABLE_END
echo global_assign_ctrl_assign_mon > /sys/fs/resctrl/info/kernel_mode || \
    fail reenable_ctrl_mon
echo SSQOSASSOC_REMOVE_BEGIN
rmdir /sys/fs/resctrl/kernel || fail remove_assigned_group
echo SSQOSASSOC_REMOVE_END
[ "$(cat /sys/fs/resctrl/info/kernel_mode_assignment)" = "none" ] || \
    fail removed_assignment
echo SSQOSASSOC_E2E_PASS
poweroff -f
"""

LEGACY_GUEST_TEST = r"""
fail() { echo SSQOSASSOC_LEGACY_FAIL:$1; poweroff -f; }
mkdir -p /sys/fs/resctrl || fail mkdir_mountpoint
mount -t resctrl -o debug resctrl /sys/fs/resctrl || fail mount
[ ! -e /sys/fs/resctrl/info/kernel_mode ] || fail kernel_mode_visible
[ ! -e /sys/fs/resctrl/info/kernel_mode_assignment ] || \
    fail assignment_visible
echo SSQOSASSOC_LEGACY_PASS
poweroff -f
"""


def path_argument(value):
    return Path(value).expanduser().resolve()


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True, type=path_argument)
    parser.add_argument("--kernel", required=True, type=path_argument)
    parser.add_argument("--initrd", required=True, type=path_argument)
    parser.add_argument("--firmware", required=True, type=path_argument)
    parser.add_argument("--opensbi", required=True, type=path_argument)
    parser.add_argument("--log", default=Path("ssqosassoc-e2e.log"),
                        type=path_argument)
    parser.add_argument("--trace", default=Path("ssqosassoc-e2e.trace"),
                        type=path_argument)
    parser.add_argument("--timeout", default=180, type=int)
    parser.add_argument("--legacy-hardware", action="store_true",
                        help="run without Ssqosassoc and verify compatibility")
    parser.add_argument("--legacy-software", action="store_true",
                        help="verify an old kernel on Ssqosassoc hardware")
    return parser.parse_args()


def qemu_args(args):
    cpu = "rv64,ssqosid=true"
    if not args.legacy_hardware:
        cpu += ",x-ssqosassoc=true"

    return [
        str(args.qemu),
        "-M", "virt,pflash0=pflash0,acpi=on",
        "-cpu", cpu,
        "-smp", "2",
        "-m", "2G",
        "-nographic",
        "-no-reboot",
        "-kernel", str(args.kernel),
        "-initrd", str(args.initrd),
        "-append",
        "root=/dev/ram rw console=ttyS0 earlycon=sbi loglevel=4 "
        "rdinit=/init iomem=relaxed",
        "-bios", str(args.opensbi),
        "-blockdev",
        "node-name=pflash0,driver=file,read-only=on,"
        f"filename={args.firmware}",
        "-device",
        "riscv.cbqri.bandwidth,max_mcids=16,max_rcids=16,"
        "nbwblks=1024,mrbwb=819,mmio_base=0x04828000",
        "-trace", "enable=riscv_qos_assoc_write",
        "-trace", f"file={args.trace}",
    ]


def clean_console(data):
    text = data.decode("utf-8", errors="replace").replace("\r", "")
    return re.sub(r"\x1b\[[0-9;=?]*[A-Za-z]", "", text)


def write_all(fd, data):
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        view = view[written:]


def send_guest_test(fd, guest_test):
    encoded = base64.b64encode(guest_test.encode()).decode()
    commands = ["stty -echo", "mkdir -p /tmp",
                ": > /tmp/ssqosassoc-e2e.b64"]
    commands.extend(
        f"echo {encoded[offset:offset + 512]} >> /tmp/ssqosassoc-e2e.b64"
        for offset in range(0, len(encoded), 512)
    )
    commands.append("base64 -d /tmp/ssqosassoc-e2e.b64 | sh")
    for command in commands:
        write_all(fd, command.encode() + b"\n")
        time.sleep(0.02)


def trace_is_valid(console, path, legacy):
    trace = path.read_text(encoding="utf-8", errors="replace")
    if legacy:
        return "riscv_qos_assoc_write" not in trace

    markers = [
        "SSQOSASSOC_DEFAULT_BEGIN",
        "SSQOSASSOC_DEFAULT_END",
        "SSQOSASSOC_ASSIGN_BOTH_BEGIN",
        "SSQOSASSOC_ASSIGN_BOTH_END",
        "SSQOSASSOC_CTRL_ONLY_BEGIN",
        "SSQOSASSOC_CTRL_ONLY_END",
        "SSQOSASSOC_DISABLE_BEGIN",
        "SSQOSASSOC_DISABLE_END",
        "SSQOSASSOC_REMOVE_BEGIN",
        "SSQOSASSOC_REMOVE_END",
    ]
    values = [
        "value:0x90011001",
        "value:0x80001001",
        "value:0x00000000",
    ]
    return all(marker in console for marker in markers) and all(
        value in trace for value in values
    )


def main():
    args = parse_args()
    legacy = args.legacy_hardware or args.legacy_software
    if args.legacy_hardware and args.legacy_software:
        print("select only one legacy mode", file=sys.stderr)
        return 2
    pass_marker = LEGACY_PASS_MARKER if legacy else PASS_MARKER
    fail_marker = ("SSQOSASSOC_LEGACY_FAIL:" if legacy else
                   "SSQOSASSOC_E2E_FAIL:")
    guest_test = LEGACY_GUEST_TEST if legacy else GUEST_TEST
    for path in (args.qemu, args.kernel, args.initrd, args.firmware,
                 args.opensbi):
        if not path.is_file():
            print(f"missing artifact: {path}", file=sys.stderr)
            return 2

    args.trace.unlink(missing_ok=True)
    master, slave = pty.openpty()
    process = subprocess.Popen(
        qemu_args(args), stdin=slave, stdout=slave, stderr=slave,
        close_fds=True
    )
    os.close(slave)
    output = bytearray()
    sent_enter = False
    sent_test = False
    deadline = time.monotonic() + args.timeout

    try:
        while time.monotonic() < deadline:
            ready, _, _ = select.select([master], [], [], 1)
            if ready:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    data = b""
                if data:
                    output.extend(data)
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()

            current = clean_console(output[-131072:])
            if not sent_enter and "Please press Enter" in current:
                write_all(master, b"\n")
                sent_enter = True
            if sent_enter and not sent_test and "[@orca ~]#" in current:
                send_guest_test(master, guest_test)
                sent_test = True
            if pass_marker in current or fail_marker in current:
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.terminate()
                break
            if process.poll() is not None:
                break
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
        os.close(master)

    result = clean_console(output)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.log.write_text(result, encoding="utf-8")
    if (pass_marker in result and fail_marker not in result and
            trace_is_valid(result, args.trace, legacy)):
        print(f"\nPASS: log written to {args.log}")
        return 0

    print(f"\nFAIL: log written to {args.log}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
