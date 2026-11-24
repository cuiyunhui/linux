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

PASS_MARKER = "IDMAP_V3_E2E_PASS:logical_ids_full_table_update_sum_release_reuse"

GUEST_TEST = r"""
fail() { echo IDMAP_E2E_FAIL:$1; poweroff -f; }
status() { echo $((($1 >> 32) & 127)); }
map_read() {
    /sbin/devmem $(($1 + 0x28)) 64 $((2 | ($2 << 8))) >/dev/null
    /sbin/devmem $(($1 + 0x28)) 64
}
map_int() {
    value=$(map_read $1 $2)
    echo $(((value >> 20) & 4095))
}
mon_read() {
    /sbin/devmem $(($1 + 0x08)) 64 $((2 | ($2 << 8))) >/dev/null
    /sbin/devmem $(($1 + 0x08)) 64
}
write_schema() {
    printf 'MB:0=%s\nMB_NODE_WEIGHT:0=%s\n' $2 $3 > /tmp/schema
    dd if=/tmp/schema of=$1/schemata bs=4096 count=1 2>/dev/null
}
check_ctrl() {
    int_rcid=$(map_int $1 $2)
    [ "$int_rcid" != "0" ] || fail map_${1}_${2}
    /sbin/devmem $(($1 + 0x18)) 64 $((2 | (int_rcid << 8))) >/dev/null
    [ "$(/sbin/devmem $(($1 + 0x20)) 64)" = "0x0000000000700004" ] || \
        fail config_${1}_${2}
}
check_ctrl_value() {
    int_rcid=$(map_int $1 $2)
    [ "$int_rcid" != "0" ] || fail map_${1}_${2}
    /sbin/devmem $(($1 + 0x18)) 64 $((2 | (int_rcid << 8))) >/dev/null
    [ "$(/sbin/devmem $(($1 + 0x20)) 64)" = "$3" ] || \
        fail config_${1}_${2}
}
for base in 0x04828000 0x04829000; do
    caps=$(/sbin/devmem $base 64)
    [ $(((caps >> 62) & 3)) = 3 ] || fail caps_$base
done
mkdir -p /sys/fs/resctrl || fail mkdir_mountpoint
mount -t resctrl none /sys/fs/resctrl 2>/dev/null || true
[ "$(cat /sys/fs/resctrl/info/MB/num_closids)" = "4096" ] || fail mb_closids
[ "$(cat /sys/fs/resctrl/info/L3_MON/num_rmids)" = "4096" ] || fail rmids
grep -q 'MB:0=0$' /sys/fs/resctrl/schemata || fail root_mb_default
grep -q 'MB_NODE_WEIGHT:0=0$' /sys/fs/resctrl/schemata || fail root_weight
mkdir -p /tmp
mkdir /sys/fs/resctrl/g1 || fail mkdir_g1
write_schema /sys/fs/resctrl/g1 25 7 || fail write_g1
root_mbm=$(cat /sys/fs/resctrl/mon_data/mon_L3_00/mbm_total_bytes)
[ "$root_mbm" = "36" ] || fail root_mbm_sum_$root_mbm
g1_mbm=$(cat /sys/fs/resctrl/g1/mon_data/mon_L3_00/mbm_total_bytes)
[ "$g1_mbm" = "36" ] || fail g1_mbm_sum_$g1_mbm
for n in 2 3 4; do
    mkdir /sys/fs/resctrl/g$n || fail mkdir_g$n
done
for n in 2 3; do
    write_schema /sys/fs/resctrl/g$n 25 7 || fail write_g$n
done
if write_schema /sys/fs/resctrl/g4 25 7; then
    fail g4_should_exhaust
fi
grep -q 'Failed to prepare control update: -28' \
    /sys/fs/resctrl/info/last_cmd_status || fail last_cmd
grep -q 'MB:0=0$' /sys/fs/resctrl/g4/schemata || fail g4_default
for base in 0x04828000 0x04829000; do
    for n in 1 2 3; do
        check_ctrl $base $n
    done
    [ "$(map_int $base 4)" = "0" ] || fail map_g4_$base
done
g1_map_0=$(map_int 0x04828000 1)
g1_map_1=$(map_int 0x04829000 1)
write_schema /sys/fs/resctrl/g1 20 9 || fail update_mapped_g1
grep -q 'MB:0=25$' /sys/fs/resctrl/g1/schemata || fail updated_g1_mb
grep -q 'MB_NODE_WEIGHT:0=9$' /sys/fs/resctrl/g1/schemata || \
    fail updated_g1_weight
[ "$(map_int 0x04828000 1)" = "$g1_map_0" ] || fail remap_g1_0
[ "$(map_int 0x04829000 1)" = "$g1_map_1" ] || fail remap_g1_1
for base in 0x04828000 0x04829000; do
    check_ctrl_value $base 1 0x0000000000900004
done
[ "$(cat /sys/fs/resctrl/g2/mon_data/mon_L3_00/mbm_total_bytes)" = \
  "Unassigned" ] || fail g2_should_unassigned
rmdir /sys/fs/resctrl/g1 || fail rmdir_g1
for base in 0x04828000 0x04829000; do
    [ "$(map_int $base 1)" = "0" ] || fail release_rcid_$base
    [ "$(status $(mon_read $base 1))" = "6" ] || fail release_mcid_$base
done
write_schema /sys/fs/resctrl/g4 20 9 || fail reuse_g4
grep -q 'MB:0=25$' /sys/fs/resctrl/g4/schemata || fail rounded_readback
grep -q 'MB_NODE_WEIGHT:0=9$' /sys/fs/resctrl/g4/schemata || fail weight_g4
for base in 0x04828000 0x04829000; do
    [ "$(map_int $base 4)" != "0" ] || fail reuse_map_$base
done
[ "$(cat /sys/fs/resctrl/g4/mon_data/mon_L3_00/mbm_total_bytes)" = "36" ] || \
    fail reuse_mcid_g4
echo IDMAP_V3_E2E_PASS:logical_ids_full_table_update_sum_release_reuse
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
        "nbwblks=16,mrbwb=12,inactive-entry=on,rcid-map=on,mcid-map=on,"
        "counter-value=17,mmio_base=0x04828000",
        "-device",
        "riscv.cbqri.bandwidth,max_mcids=2,max_rcids=4,"
        "nbwblks=16,mrbwb=12,inactive-entry=on,rcid-map=on,mcid-map=on,"
        "counter-value=19,mmio_base=0x04829000",
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
