#ifndef LINUX_DNUMA_H_
#define LINUX_DNUMA_H_
#define DEBUG 1

#include <linux/mm.h>

#ifdef CONFIG_DYNAMIC_NUMA
#include <linux/memlayout.h>
#include <linux/spinlock.h>

extern u64 dnuma_moved_page_ct;
extern spinlock_t dnuma_stats_lock;

/* called by memlayout_commit() when a new memory layout takes effect. */
static inline void dnuma_move_to_new_ml(struct memlayout *new_ml)
{
	/* allocate a per node list of pages */

	int nid;
	/* XXX: locking considerations:
	 *  - what can cause the hotplugging of a node? Do we just need to lock_memory_hotplug()?
	 *  - pgdat_resize_lock()
	 *    - "Nests above zone->lock and zone->size_seqlock."
	 *  - zone_span_seq*() & zone_span_write*() */
	for_each_online_node(nid) {
		/*
		 *	lock the node
		 *		pull out pages that don't belong to it anymore, put them on the list.
		 *		update spanned_pages, start pfn, free pages
		 *	unlock the node
		 */
	}

	for_each_online_node(nid) {
		/* add pages that are new to the node (and removed in the previous
		 * iteration from other nodes) to it's free lists. (appropriate locking).
		 */
	}
}

static inline struct zone *get_zone(int nid, enum zone_type zonenum)
{
	return &NODE_DATA(nid)->node_zones[zonenum];
}

void dnuma_adjust_spanned_pages(unsigned long pfn,
		struct zone *new_zone, struct pglist_data *new_node);

static inline bool dnuma_page_needs_move(struct page *page, int *dest_nid)
{
	unsigned long pfn = page_to_pfn(page);
	int new_nid = memlayout_pfn_to_nid_no_pageflags(pfn);
	int old_nid = page_to_nid(page);

	if (new_nid == NUMA_NO_NODE || new_nid == old_nid)
		return false;

	if (dest_nid)
		*dest_nid = new_nid;

	return true;
}

static inline struct zone *dnuma_pre_free_to_new_zone(struct page *page, int dest_nid)
{
	struct zone *dest_zone = get_zone(dest_nid, page_zonenum(page));
	set_page_node(page, dest_nid);
	dnuma_adjust_spanned_pages(page_to_pfn(page),
			dest_zone, NODE_DATA(dest_nid));
	return dest_zone;
}

/* moves a page when it is being freed (and thus was not moved by
 * dnuma_move_free_pages()) */
static inline struct zone *dnuma_move_free_page_zone(struct page *page)
{
	/* XXX: handle pageblock_flags */
	enum zone_type zonenum = page_zonenum(page);
	unsigned long pfn = page_to_pfn(page);
	int new_nid = memlayout_pfn_to_nid_no_pageflags(pfn);
	int old_nid = page_to_nid(page);

	if (new_nid == NUMA_NO_NODE)
		new_nid = old_nid;
	if (new_nid != old_nid) {
		unsigned long flags;
		dnuma_pre_free_to_new_zone(page, new_nid);
		spin_lock_irqsave(&dnuma_stats_lock, flags);
		if (dnuma_moved_page_ct < (~(u64)0))
			dnuma_moved_page_ct ++;
		spin_unlock_irqrestore(&dnuma_stats_lock, flags);
	}

	return get_zone(new_nid, zonenum);
}
#else
static inline struct zone *dnuma_move_free_page_zone(struct page *page)
{ return page_zone(page); }
#endif

#endif
