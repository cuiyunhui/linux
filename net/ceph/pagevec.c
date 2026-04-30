// SPDX-License-Identifier: GPL-2.0
#include <linux/ceph/ceph_debug.h>

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/writeback.h>

#include <linux/ceph/libceph.h>

void ceph_put_page_vector(struct page **pages, int num_pages, bool dirty)
{
	int i;

	for (i = 0; i < num_pages; i++) {
		if (dirty)
			set_page_dirty_lock(pages[i]);
		put_page(pages[i]);
	}
	kvfree(pages);
}
EXPORT_SYMBOL(ceph_put_page_vector);

void ceph_release_page_vector(struct page **pages, int num_pages)
{
	int i;

	for (i = 0; i < num_pages; i++)
		__free_pages(pages[i], 0);
	kfree(pages);
}
EXPORT_SYMBOL(ceph_release_page_vector);

/*
 * allocate a vector new pages
 */
struct page **ceph_alloc_page_vector(int num_pages, gfp_t flags)
{
	struct page **pages;
	int i;

	pages = kmalloc_objs(*pages, num_pages, flags);
	if (!pages)
		return ERR_PTR(-ENOMEM);
	for (i = 0; i < num_pages; i++) {
		pages[i] = __page_cache_alloc(flags);
		if (pages[i] == NULL) {
			ceph_release_page_vector(pages, i);
			return ERR_PTR(-ENOMEM);
		}
	}
	return pages;
}
EXPORT_SYMBOL(ceph_alloc_page_vector);

void ceph_copy_from_page_vector(struct page **pages,
				    void *data,
				    loff_t off, size_t len)
{
	int i = 0;
	size_t po = off & ~PG_MASK;
	size_t left = len;

	while (left > 0) {
		size_t l = min_t(size_t, PG_SIZE-po, left);

		memcpy(data, page_address(pages[i]) + po, l);
		data += l;
		left -= l;
		po += l;
		if (po == PG_SIZE) {
			po = 0;
			i++;
		}
	}
}
EXPORT_SYMBOL(ceph_copy_from_page_vector);

/*
 * Zero an extent within a page vector.  Offset is relative to the
 * start of the first page.
 */
void ceph_zero_page_vector_range(int off, int len, struct page **pages)
{
	int i = off >> PG_SHIFT;

	off &= ~PG_MASK;

	dout("zero_page_vector_page %u~%u\n", off, len);

	/* leading partial page? */
	if (off) {
		int end = min((int) PG_SIZE, off + len);
		dout("zeroing %d %p head from %d\n", i, pages[i],
		     (int)off);
		zero_user_segment(pages[i], off, end);
		len -= (end - off);
		i++;
	}
	while (len >= PG_SIZE) {
		dout("zeroing %d %p len=%d\n", i, pages[i], len);
		zero_user_segment(pages[i], 0, PG_SIZE);
		len -= PG_SIZE;
		i++;
	}
	/* trailing partial page? */
	if (len) {
		dout("zeroing %d %p tail to %d\n", i, pages[i], (int)len);
		zero_user_segment(pages[i], 0, len);
	}
}
EXPORT_SYMBOL(ceph_zero_page_vector_range);
