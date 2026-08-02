// SPDX-License-Identifier: GPL-2.0-only
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pgtable.h>
#include <linux/uaccess.h>

#include "svnapot_probe_uapi.h"

static int svnapot_read_ptes(struct mm_struct *mm,
			     struct svnapot_probe_query *query)
{
	unsigned long addr = query->base;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep;
	spinlock_t *ptl; /* Protects the PTE page. */
	unsigned int i;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(pgdp_get(pgd)) || pgd_bad(pgdp_get(pgd)))
		return -ENOENT;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(p4dp_get(p4d)) || p4d_bad(p4dp_get(p4d)))
		return -ENOENT;
	pud = pud_offset(p4d, addr);
	if (pud_none(pudp_get(pud)) || pud_bad(pudp_get(pud)) ||
	    pud_leaf(pudp_get(pud)))
		return -ENOENT;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(pmdp_get(pmd)) || pmd_bad(pmdp_get(pmd)) ||
	    pmd_leaf(pmdp_get(pmd)))
		return -ENOENT;

	ptl = pte_lockptr(mm, pmd);
	spin_lock(ptl);
	ptep = pte_offset_kernel(pmd, addr);
	for (i = 0; i < SVNAPOT_PROBE_PTES; i++, addr += PTE_SIZE) {
		pte_t logical, pte = READ_ONCE(ptep[i]);

		query->raw[i] = pte_val(pte);
		query->seen_mask |= BIT(i);
		if (pte_present(pte)) {
			query->present_mask |= BIT(i);
			logical = pte_mknonnapot(pte, addr);
			query->pfn[i] = pte_pfn(logical);
		}
		if (pte_napot(pte))
			query->napot_mask |= BIT(i);
	}
	spin_unlock(ptl);

	return 0;
}

static long svnapot_query(unsigned long arg)
{
	struct svnapot_probe_query query;
	struct mm_struct *mm = current->mm;
	int ret;

	if (!mm)
		return -EINVAL;
	if (copy_from_user(&query, (void __user *)arg, sizeof(query)))
		return -EFAULT;

	query.base = ALIGN_DOWN(query.addr, SVNAPOT_PROBE_SIZE);
	memset(query.raw, 0, sizeof(query.raw));
	memset(query.pfn, 0, sizeof(query.pfn));
	query.seen_mask = 0;
	query.present_mask = 0;
	query.napot_mask = 0;
	query.pte_shift = PTE_SHIFT;
	query.pg_shift = PG_SHIFT;
	query.reserved = 0;

	mmap_read_lock(mm);
	ret = svnapot_read_ptes(mm, &query);
	mmap_read_unlock(mm);
	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &query, sizeof(query)))
		return -EFAULT;

	return 0;
}

static long svnapot_pin(unsigned long arg)
{
	struct svnapot_probe_pin pin;
	struct page **pages;
	unsigned long nr;
	unsigned int gup_flags = 0;
	long pinned;

	if (copy_from_user(&pin, (void __user *)arg, sizeof(pin)))
		return -EFAULT;
	if (!pin.length || pin.length > SVNAPOT_PROBE_SIZE)
		return -EINVAL;
	if (pin.flags & ~SVNAPOT_PROBE_PIN_WRITE)
		return -EINVAL;
	if (pin.flags & SVNAPOT_PROBE_PIN_WRITE)
		gup_flags |= FOLL_WRITE;

	nr = DIV_ROUND_UP(offset_in_pte(pin.addr) + pin.length, PTE_SIZE);
	if (nr > SVNAPOT_PROBE_PTES)
		return -EINVAL;

	pages = kcalloc(nr, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	pinned = pin_user_pages_fast(pin.addr, nr, gup_flags, pages);
	if (pinned > 0)
		unpin_user_pages(pages, pinned);
	kfree(pages);

	pin.pinned = pinned;
	if (copy_to_user((void __user *)arg, &pin, sizeof(pin)))
		return -EFAULT;

	return pinned < 0 ? pinned : 0;
}

static long svnapot_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	(void)file;

	switch (cmd) {
	case SVNAPOT_PROBE_QUERY:
		return svnapot_query(arg);
	case SVNAPOT_PROBE_PIN:
		return svnapot_pin(arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations svnapot_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = svnapot_ioctl,
};

static struct miscdevice svnapot_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "svnapot_probe",
	.fops = &svnapot_fops,
	.mode = 0600,
};

module_misc_device(svnapot_device);

MODULE_DESCRIPTION("RISC-V Svnapot raw PTE selftest probe");
MODULE_LICENSE("GPL");
