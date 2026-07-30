// SPDX-License-Identifier: GPL-2.0-only

#include <linux/align.h>
#include <linux/cpufeature.h>
#include <linux/export.h>
#include <linux/pgtable.h>

static inline bool napot_hw_supported(void)
{
	return riscv_has_extension_unlikely(RISCV_ISA_EXT_SVNAPOT);
}

static inline unsigned int napotpte_order(void)
{
	return NAPOT_CONT64KB_ORDER;
}

static inline unsigned int napotpte_pte_num(void)
{
	return napot_pte_num(napotpte_order());
}

static inline pte_t *napot_align_ptep(pte_t *ptep)
{
	return PTR_ALIGN_DOWN(ptep, napotpte_pte_num() * sizeof(*ptep));
}

static inline pte_t pte_mask_ad(pte_t pte)
{
	return pte_mkold(pte_mkclean(pte));
}

static inline unsigned long pte_protval_no_pfn_no_napot(pte_t pte)
{
	return (pte_val(pte) & ~_PAGE_PFN_MASK) & ~_PAGE_NAPOT;
}

static inline pte_t napotpte_subpte(pte_t *ptep, pte_t pte)
{
	unsigned long pfn;
	pgprot_t prot;

	if (!pte_present_napot(pte))
		return pte;

	pfn = pte_pfn(pte) + (ptep - napot_align_ptep(ptep));
	prot = __pgprot(pte_protval_no_pfn_no_napot(pte));

	return pfn_pte(pfn, prot);
}

static inline bool napotpte_is_consistent(pte_t pte, pte_t orig_pte)
{
	return pte_present_napot(pte) &&
	       pte_val(pte_mask_ad(pte)) == pte_val(pte_mask_ad(orig_pte));
}

pte_t napotpte_ptep_get(pte_t *ptep, pte_t orig_pte)
{
	pte_t pte, cur;
	pte_t *start;
	unsigned int i, nr;

	if (!napot_hw_supported() || !pte_present_napot(orig_pte))
		return orig_pte;

	pte = orig_pte;
	start = napot_align_ptep(ptep);
	nr = napotpte_pte_num();

	/*
	 * ptep_get() is called with the PTL held, so the block cannot be
	 * converted while accessed and dirty state is collected.
	 */
	for (i = 0; i < nr; i++) {
		cur = READ_ONCE(start[i]);
		if (pte_dirty(cur)) {
			pte = riscv_pte_mkhwdirty(pte);
			for (; i < nr; i++) {
				cur = READ_ONCE(start[i]);
				if (pte_young(cur)) {
					pte = pte_mkyoung(pte);
					break;
				}
			}
			break;
		}

		if (pte_young(cur)) {
			pte = pte_mkyoung(pte);
			i++;
			for (; i < nr; i++) {
				cur = READ_ONCE(start[i]);
				if (pte_dirty(cur)) {
					pte = riscv_pte_mkhwdirty(pte);
					break;
				}
			}
			break;
		}
	}

	return napotpte_subpte(ptep, pte);
}
EXPORT_SYMBOL(napotpte_ptep_get);

pte_t napotpte_ptep_get_lockless(pte_t *orig_ptep)
{
	pte_t orig_pte, pte;
	pte_t *ptep;
	unsigned int i, nr;

	if (!napot_hw_supported())
		return READ_ONCE(*orig_ptep);

	nr = napotpte_pte_num();

retry:
	orig_pte = READ_ONCE(*orig_ptep);
	if (!pte_present_napot(orig_pte))
		return orig_pte;

	ptep = napot_align_ptep(orig_ptep);

	for (i = 0; i < nr; i++, ptep++) {
		pte = READ_ONCE(*ptep);

		if (!napotpte_is_consistent(pte, orig_pte))
			goto retry;

		if (pte_dirty(pte)) {
			orig_pte = riscv_pte_mkhwdirty(orig_pte);
			for (; i < nr; i++, ptep++) {
				pte = READ_ONCE(*ptep);

				if (!napotpte_is_consistent(pte, orig_pte))
					goto retry;

				if (pte_young(pte)) {
					orig_pte = pte_mkyoung(orig_pte);
					break;
				}
			}
			break;
		}

		if (pte_young(pte)) {
			orig_pte = pte_mkyoung(orig_pte);
			i++;
			ptep++;
			for (; i < nr; i++, ptep++) {
				pte = READ_ONCE(*ptep);

				if (!napotpte_is_consistent(pte, orig_pte))
					goto retry;

				if (pte_dirty(pte)) {
					orig_pte = riscv_pte_mkhwdirty(orig_pte);
					break;
				}
			}
			break;
		}
	}

	return napotpte_subpte(orig_ptep, orig_pte);
}
EXPORT_SYMBOL(napotpte_ptep_get_lockless);
