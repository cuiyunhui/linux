#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Apply scripts/pte-size-rename.cocci treewide while excluding the mm RFC
# scope (hand-curated by the PTE_SIZE vs PG_SIZE split series). Run this
# from the top of the tree after the mm RFC block is applied, then commit
# the result as the "treewide: rename PAGE_SIZE/SHIFT/MASK to PG_* (cocci)"
# patch.
#
# Requires spatch >= 1.3.0. Pass extra args through to spatch (e.g. --debug).
#
# The tree is walked one top-level directory at a time because spatch's
# --dir . + --use-gitgrep combination scales poorly and errors out on the
# whole kernel tree. Per-dir invocation is slower but reliable.

set -e

cd "$(git rev-parse --show-toplevel)"

if ! command -v spatch >/dev/null; then
	echo "scripts/pte-size-regen.sh: spatch not found;" >&2
	echo "  try:  nix-shell -p coccinelle --run '$0 $*'" >&2
	exit 127
fi

# Top-level directories to process. mm/ now participates: the page-fault
# handler is hand-curated per-occurrence with deliberate PG/PTE-unit
# choices, and cocci picks up the remaining non-fault references.
DIRS="
	arch
	block
	certs
	crypto
	drivers
	fs
	include
	init
	io_uring
	ipc
	kernel
	lib
	mm
	net
	rust
	samples
	scripts
	security
	sound
	tools
	virt
"

# Headers inside include/linux/ the RFC mm series hand-curates — cocci must
# not touch these. Keep in sync with the header comment in
# scripts/pte-size-rename.cocci.
IGNORES="
	--ignore include/linux/mm.h
	--ignore include/linux/mm_types.h
	--ignore include/linux/pfn.h
	--ignore include/linux/pagemap.h
	--ignore include/linux/pgtable.h
	--ignore include/linux/rmap.h
	--ignore include/linux/highmem.h
"

for dir in $DIRS; do
	[ -d "$dir" ] || continue
	echo "==> $dir"
	# Redirect spatch output to a log rather than piping — a bounded pipe
	# (e.g. | head -N) sends SIGPIPE and prematurely kills spatch when
	# the consumer closes early.
	spatch \
		--sp-file scripts/pte-size-rename.cocci \
		--dir "$dir" \
		--use-gitgrep \
		--in-place \
		--no-show-diff \
		--include-headers \
		--jobs "$(nproc)" \
		$IGNORES \
		"$@" >"/tmp/pte-size-regen-$dir.log" 2>&1 \
		|| echo "    (spatch returned non-zero; see /tmp/pte-size-regen-$dir.log)"
done
