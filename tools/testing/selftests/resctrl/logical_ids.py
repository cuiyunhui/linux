#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate resctrl logical ID allocation on a selected cache domain."""

import argparse
import errno
import os
from pathlib import Path
import subprocess
import sys

KSFT_SKIP = 4


def schemas(path):
    result = {}
    for line in path.read_text().splitlines():
        if ":" not in line:
            continue
        name, values = line.split(":", 1)
        result[name.strip()] = dict(item.split("=", 1)
                                    for item in values.strip().split(";"))
    return result


def write_policy(group, names, domain, values):
    text = "".join(f"{name}:{domain}={values[name]}\n" for name in names)
    (group / "schemata").write_text(text)


def policy(group, names, domain):
    current = schemas(group / "schemata")
    return {name: current[name][domain] for name in names}


def expect_enospc(action):
    try:
        action()
    except OSError as error:
        if error.errno == errno.ENOSPC:
            return
        raise
    raise AssertionError("operation succeeded despite exhausted capacity")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("identity", "local"))
    parser.add_argument("--root", type=Path, default=Path("/sys/fs/resctrl"))
    parser.add_argument("--resource", default="L3")
    parser.add_argument("--domain")
    parser.add_argument("--physical-entries", type=int)
    parser.add_argument("--groups", type=int, default=64)
    args = parser.parse_args()
    if args.mode is None:
        print("1..0 # SKIP select --mode and a suitable cache domain")
        return KSFT_SKIP
    if os.geteuid() != 0:
        print("1..0 # SKIP requires root")
        return KSFT_SKIP
    if args.mode == "local" and args.physical_entries is None:
        parser.error("--mode local requires --physical-entries")
    root = args.root
    mounted_here = False
    groups = []
    root_changed = False
    original = {}
    names = []
    domain = ""
    case = 0

    def passed(description):
        nonlocal case
        case += 1
        print(f"ok {case} - {description}", flush=True)

    try:
        root.mkdir(parents=True, exist_ok=True)
        if not (root / "schemata").exists():
            subprocess.run(["mount", "-t", "resctrl", "resctrl", str(root)],
                           check=True)
            mounted_here = True
        if any(path.is_dir() and (path / "tasks").exists()
               for path in root.iterdir()):
            print("1..0 # SKIP another control group is present")
            return KSFT_SKIP
        available = schemas(root / "schemata")
        if args.resource in available:
            names = [args.resource]
        elif (args.resource + "DATA" in available and
              args.resource + "CODE" in available):
            names = [args.resource + "DATA", args.resource + "CODE"]
        else:
            print("1..0 # SKIP requested cache control is unavailable")
            return KSFT_SKIP
        domain = args.domain or next(iter(available[names[0]]))
        original = {name: available[name][domain] for name in names}
        namespace = int((root / "info" / names[0] /
                         "num_closids").read_text())
        paired = len(names) == 2
        if args.mode == "local":
            roots = 2 if paired else 1
            policies = (args.physical_entries - roots) // len(names)
            count = min(namespace - 1, max(args.groups, policies + 1))
            if policies < 1 or count <= policies:
                print("1..0 # SKIP no spare logical ID beyond physical capacity")
                return KSFT_SKIP
        else:
            policies = namespace - 1
            count = policies
        if count < 2:
            print("1..0 # SKIP at least two control groups are required")
            return KSFT_SKIP

        print("TAP version 13\n1..5", flush=True)
        for number in range(count):
            group = root / f"logical-id-{os.getpid()}-{number}"
            group.mkdir()
            groups.append(group)
            if policy(group, names, domain) != original:
                raise AssertionError("new group did not inherit root policy")
        if args.mode == "identity":
            extra = root / f"logical-id-{os.getpid()}-overflow"
            expect_enospc(extra.mkdir)
        passed("create logical groups with inherited policy")

        write_policy(groups[0], names, domain, original)
        if policy(groups[0], names, domain) != original:
            raise AssertionError("same-value write changed the policy")
        passed("accept an explicit same-value policy")

        minimum = root / "info" / names[0] / "min_cbm_bits"
        width = int(minimum.read_text()) if minimum.exists() else 2
        changed = {}
        for name, value in original.items():
            mask = int(value, 16)
            lowest = (mask & -mask).bit_length() - 1
            candidate = ((1 << width) - 1) << lowest
            if candidate == mask or candidate & mask != candidate:
                raise AssertionError("cannot construct a smaller valid mask")
            changed[name] = f"{candidate:x}"
        root_changed = True
        write_policy(root, names, domain, changed)
        if policy(groups[0], names, domain) != original:
            raise AssertionError("explicit policy followed a root update")
        expected = changed if args.mode == "local" else original
        if policy(groups[1], names, domain) != expected:
            raise AssertionError("unexpected default inheritance behavior")
        write_policy(root, names, domain, original)
        root_changed = False
        passed("separate shared defaults from independent policies")

        for group in groups[1:policies]:
            write_policy(group, names, domain, original)
        if args.mode == "local":
            pending = groups[policies]
            expect_enospc(lambda: write_policy(pending, names, domain,
                                               original))
            if policy(pending, names, domain) != original:
                raise AssertionError("failed policy changed root fallback")
        passed("report physical exhaustion or retain identity capacity")

        released = groups.pop(0)
        released.rmdir()
        if args.mode == "local":
            write_policy(pending, names, domain, original)
            if policy(pending, names, domain) != original:
                raise AssertionError("retry did not apply the policy")
        else:
            replacement = root / f"logical-id-{os.getpid()}-replacement"
            replacement.mkdir()
            groups.append(replacement)
            write_policy(replacement, names, domain, original)
        passed("release an entry and retry or reuse the identity ID")
        return 0
    except (OSError, ValueError, AssertionError,
            subprocess.CalledProcessError) as error:
        if case < 5:
            print(f"not ok {case + 1} - {error}", flush=True)
            for remaining in range(case + 2, 6):
                print(f"ok {remaining} # SKIP earlier failure", flush=True)
        else:
            print(f"# cleanup failure: {error}", flush=True)
        return 1
    finally:
        if root_changed:
            write_policy(root, names, domain, original)
        for group in reversed(groups):
            if group.exists():
                group.rmdir()
        if mounted_here:
            subprocess.run(["umount", str(root)], check=True)


if __name__ == "__main__":
    sys.exit(main())
