// SPDX-License-Identifier: GPL-2.0-only
//
/// Rename PAGE_SIZE/PAGE_SHIFT/PAGE_MASK/PAGE_ALIGN/PAGE_ALIGN_DOWN and
/// offset_in_page() to their PG_* / offset_in_pg() equivalents, as part of
/// the PTE_SIZE vs PG_SIZE split (see Documentation/mm/pte-size-fault-handling.rst).
///
/// The mm core and its RFC-scope headers are hand-curated — cocci must not
/// touch them, since the RFC patchset makes deliberate PG vs PTE choices
/// there. Invoke spatch with the wrapper script scripts/pte-size-regen.sh,
/// which passes --include-headers-for-types and --ignore for:
///
///   mm/**
///   include/linux/mm.h
///   include/linux/mm_types.h
///   include/linux/pfn.h
///   include/linux/pagemap.h
///   include/linux/pgtable.h
///   include/linux/rmap.h
///   include/linux/highmem.h
///
/// Expected usage (from top of tree, after the mm RFC block is applied):
///
///   scripts/pte-size-regen.sh
///
/// which runs this file over the tree, writes the changes in place, and
/// leaves the diff ready for `git commit -a`.

@@ @@
- PAGE_SIZE
+ PG_SIZE

@@ @@
- PAGE_SHIFT
+ PG_SHIFT

@@ @@
- PAGE_MASK
+ PG_MASK

@@ @@
- PAGE_ALIGN_DOWN
+ PG_ALIGN_DOWN

@@ @@
- PAGE_ALIGN
+ PG_ALIGN

@@ @@
- PAGE_ALIGNED
+ PG_ALIGNED

@@ expression E; @@
- offset_in_page(E)
+ offset_in_pg(E)
