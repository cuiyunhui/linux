#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the CBQRI identifier-mapping QEMU/Linux end-to-end test."""

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

PASS_MARKER = "IDMAP_E2E_PASS:logical_ids_sparse_rcid_mcid_exhaust_release_reuse"

GUEST_TEST = r"""
fail() { echo IDMAP_E2E_FAIL:$1; poweroff -f; }
status() { echo $((($1 >> 32) & 127)); }
map_read() {
    /sbin/devmem 0x04828028 64 $((2 | ($1 << 8))) >/dev/null
    /sbin/devmem 0x04828028 64
}
mon_config() {
    /sbin/devmem 0x04828008 64 $((1 | ($1 << 8) | ($2 << 20))) >/dev/null
    /sbin/devmem 0x04828008 64
}
mon_read() {
    /sbin/devmem 0x04828008 64 $((2 | ($1 << 8))) >/dev/null
    /sbin/devmem 0x04828008 64
}
caps=$(/sbin/devmem 0x04828000 64)
[ "$caps" = "0xC000033300040012" ] || fail caps_$caps
mkdir -p /sys/fs/resctrl || fail mkdir_mountpoint
mount -t resctrl -o debug resctrl /sys/fs/resctrl || fail mount
[ "$(cat /sys/fs/resctrl/info/MB/num_closids)" = "4096" ] || fail mb_closids
[ "$(cat /sys/fs/resctrl/info/MWEIGHT/num_closids)" = "4096" ] || fail mw_closids
[ "$(cat /sys/fs/resctrl/info/MB_MON/num_rmids)" = "4096" ] || fail rmids
grep -q '^     MB:0=80$' /sys/fs/resctrl/schemata || fail root_mb_default
grep -q '^MWEIGHT:0=255$' /sys/fs/resctrl/schemata || fail root_mw_default
[ "$(status $(mon_read 0))" = "6" ] || fail reset_mcid0_not_released
[ "$(status $(mon_read 1))" = "6" ] || fail reset_mcid1_not_released
echo 'MWEIGHT:0=0' > /sys/fs/resctrl/schemata || fail root_mweight_zero
echo 'MB:0=0' > /sys/fs/resctrl/schemata || fail root_mb_zero
for n in 1 2 3 4 5; do
    mkdir /sys/fs/resctrl/g$n || fail mkdir_g$n
    [ "$(cat /sys/fs/resctrl/g$n/mon_hw_id)" = "$n" ] || fail rmid_g$n
done
for n in 1 2 3; do
    echo 'MB:0=20' > /sys/fs/resctrl/g$n/schemata || fail mb_g$n
done
if echo 'MB:0=30' > /sys/fs/resctrl/g4/schemata; then
    fail g4_should_exhaust
fi
grep -q 'No free hardware control ID for MB domain 0; CLOSID 4' \
    /sys/fs/resctrl/info/last_cmd_status || fail last_cmd
grep -q '^     MB:0=0$' /sys/fs/resctrl/g4/schemata || fail g4_default
[ "$(map_read 1)" = "0x0000000100100102" ] || fail map_g1
[ "$(map_read 2)" = "0x0000000100200202" ] || fail map_g2
[ "$(map_read 3)" = "0x0000000100300302" ] || fail map_g3
[ "$(map_read 4)" = "0x0000000100000402" ] || fail map_g4_default
[ "$(status $(mon_config 1 1))" = "1" ] || fail bind_mcid1
[ "$(status $(mon_config 2 1))" = "1" ] || fail bind_mcid2
[ "$(status $(mon_config 3 1))" = "6" ] || fail mcid3_should_exhaust
[ "$(status $(mon_read 3))" = "6" ] || fail mcid3_should_unassigned
[ "$(/sbin/devmem 0x04828010 64)" = "0x4000000000000000" ] || fail mcid3_inv
rmdir /sys/fs/resctrl/g1 || fail rmdir_g1
[ "$(map_read 1)" = "0x0000000100000102" ] || fail rcid1_not_released
[ "$(status $(mon_read 1))" = "6" ] || fail mcid1_not_released
echo 'MB:0=30' > /sys/fs/resctrl/g4/schemata || fail reuse_rcid_g4
grep -q '^     MB:0=30$' /sys/fs/resctrl/g4/schemata || fail percent_readback
[ "$(map_read 4)" = "0x0000000100100402" ] || fail rcid_reuse_g4
[ "$(status $(mon_config 3 1))" = "1" ] || fail reuse_counter_mcid3
[ "$(status $(mon_read 3))" = "1" ] || fail read_mcid3
echo IDMAP_E2E_PASS:logical_ids_sparse_rcid_mcid_exhaust_release_reuse
poweroff -f
"""


def path_argument(value):
    return Path(value).expanduser().resolve()


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True, type=path_argument,
                        help="QEMU system-riscv64 binary")
    parser.add_argument("--kernel", required=True, type=path_argument,
                        help="RISC-V Linux Image")
    parser.add_argument("--initrd", required=True, type=path_argument,
                        help="initramfs containing sh, base64 and /sbin/devmem")
    parser.add_argument("--firmware", required=True, type=path_argument,
                        help="EDK2 RISC-V code image")
    parser.add_argument("--opensbi", required=True, type=path_argument,
                        help="OpenSBI fw_dynamic binary")
    parser.add_argument("--log", default=Path("cbqri-idmap-e2e.log"),
                        type=path_argument, help="console log output path")
    parser.add_argument("--timeout", default=180, type=int,
                        help="timeout in seconds (default: 180)")
    return parser.parse_args()


def qemu_args(args):
    return [
        str(args.qemu),
        "-M", "virt,pflash0=pflash0,acpi=on",
        "-cpu", "max",
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
        "riscv.cbqri.bandwidth,max_mcids=2,max_rcids=4,"
        "nbwblks=1024,mrbwb=819,inactive-entry=on,"
        "rcid-map=on,mcid-map=on,mmio_base=0x04828000",
    ]


def clean_console(data):
    text = data.decode("utf-8", errors="replace").replace("\r", "")
    return re.sub(r"\x1b\[[0-9;=?]*[A-Za-z]", "", text)


def write_all(fd, data):
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        view = view[written:]


def send_guest_test(fd):
    encoded = base64.b64encode(GUEST_TEST.encode()).decode()
    commands = ["stty -echo", "mkdir -p /tmp", ": > /tmp/idmap-e2e.b64"]
    commands.extend(
        f"echo {encoded[offset:offset + 512]} >> /tmp/idmap-e2e.b64"
        for offset in range(0, len(encoded), 512)
    )
    commands.append("base64 -d /tmp/idmap-e2e.b64 | sh")
    for command in commands:
        write_all(fd, command.encode() + b"\n")
        time.sleep(0.02)


def main():
    args = parse_args()
    for path in (args.qemu, args.kernel, args.initrd, args.firmware, args.opensbi):
        if not path.is_file():
            print(f"missing artifact: {path}", file=sys.stderr)
            return 2

    master, slave = pty.openpty()
    process = subprocess.Popen(
        qemu_args(args), stdin=slave, stdout=slave, stderr=slave, close_fds=True
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
            if not sent_enter and "Please press Enter to activate this console." in current:
                write_all(master, b"\n")
                sent_enter = True
            if sent_enter and not sent_test and "[@orca ~]#" in current:
                send_guest_test(master)
                sent_test = True
            if PASS_MARKER in current or "IDMAP_E2E_FAIL:" in current:
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
    if PASS_MARKER in result and "IDMAP_E2E_FAIL:" not in result:
        print(f"\nPASS: log written to {args.log}")
        return 0

    print(f"\nFAIL: log written to {args.log}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
