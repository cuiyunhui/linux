#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Boot a kernel with the PTE_SIZE test initrd and capture output.
#
# Usage:
#   ./boot-test.sh [bzImage-path] [-- extra qemu args]
#
# Environment:
#   TIMEOUT      - boot timeout in seconds (default: 60)
#   QEMU_EXTRA   - additional QEMU kernel params
#   LOG          - log file path (default: /tmp/pte-test-boot.log)
#
# Examples:
#   ./boot-test.sh ../../build-full/arch/x86/boot/bzImage
#   ./boot-test.sh ../../build-4k/arch/x86/boot/bzImage
#   TIMEOUT=120 LOG=/tmp/64k.log ./boot-test.sh ../../build-full/arch/x86/boot/bzImage

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TIMEOUT="${TIMEOUT:-60}"
LOG="${LOG:-/tmp/pte-test-boot.log}"

# Parse args
KERNEL=""
QEMU_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --)
            shift
            QEMU_ARGS=("$@")
            break
            ;;
        *)
            KERNEL="$1"
            shift
            ;;
    esac
done

# Default kernel path
if [[ -z "$KERNEL" ]]; then
    # Try common locations
    for k in \
        "$SCRIPT_DIR/../../../build-full/arch/x86/boot/bzImage" \
        "$SCRIPT_DIR/../../../build/arch/x86/boot/bzImage" \
        "$SCRIPT_DIR/../../../build-4k/arch/x86/boot/bzImage"; do
        if [[ -f "$k" ]]; then
            KERNEL="$k"
            break
        fi
    done
fi

if [[ -z "$KERNEL" || ! -f "$KERNEL" ]]; then
    echo "Error: kernel image not found"
    echo "Usage: $0 <bzImage-path>"
    exit 1
fi

KERNEL="$(realpath "$KERNEL")"

# Find QEMU
QEMU="${QEMU:-}"
if [[ -z "$QEMU" ]]; then
    QEMU="$(command -v qemu-system-x86_64 2>/dev/null || true)"
fi
if [[ -z "$QEMU" ]]; then
    # Look in nix store (the VM script's QEMU)
    QEMU="$(ls /nix/store/*qemu-host-cpu-only*/bin/qemu-system-x86_64 2>/dev/null | head -1 || true)"
fi
if [[ -z "$QEMU" ]]; then
    QEMU="$(ls /nix/store/*qemu-*/bin/qemu-system-x86_64 2>/dev/null | head -1 || true)"
fi
if [[ -z "$QEMU" || ! -x "$QEMU" ]]; then
    echo "Error: qemu-system-x86_64 not found"
    echo "Set QEMU= or install qemu"
    exit 1
fi

# Build the initrd
echo "Building test initrd..."
INITRD=$(cd "$SCRIPT_DIR" && nix build .#initrd --no-link --print-out-paths 2>/dev/null)
echo "Initrd: $INITRD ($(du -h "$INITRD" | cut -f1))"
echo "Kernel: $KERNEL"
echo "QEMU:   $QEMU"
echo "Log:    $LOG"
echo ""

KPARAMS="console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 nokaslr nosmp norandmaps panic=-1 ${QEMU_EXTRA:-}"

echo "Booting (timeout=${TIMEOUT}s)..."
echo "---"

# Use timeout to kill QEMU if it hangs
set +e
timeout "$TIMEOUT" "$QEMU" \
    -machine accel=kvm:tcg -cpu max \
    -m 512 \
    -smp 1 \
    -nographic \
    -no-reboot \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -append "$KPARAMS" \
    "${QEMU_ARGS[@]}" \
    </dev/null 2>&1 | tee "$LOG"
RC=$?
set -e

echo ""
echo "---"

if [[ $RC -eq 124 ]]; then
    echo "TIMEOUT: VM did not shut down within ${TIMEOUT}s"
elif [[ $RC -ne 0 ]]; then
    echo "QEMU exited with code $RC"
fi

# Extract test results from log
if grep -q "Results:" "$LOG"; then
    echo ""
    grep "Results:" "$LOG"
fi

exit $RC
