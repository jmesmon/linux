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
		/*	allocate a new pageblock_flags
		 *	lock the node
		 *		pull out pages that don't belong to it anymore, put them on the list.
		 *		give it a new pageblock_flags.
		 *		update spanned_pages, start pfn, free pages
		 *	unlock the node
		 *	free old pageblock_flags
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

/* Note: growth of spanned pages is not allowed if !defined(CONFIG_SPARSEMEM)
 * due to pageblock_flags */
static inline void dnuma_adjust_spanned_pages(unsigned long pfn,
		struct zone *old_zone, struct pglist_data *old_node,
		struct zone *new_zone, struct pglist_data *new_node)
{
	unsigned long flags, end_pfn;
	pgdat_resize_lock(new_node, &flags);

	/* grow new zone */
	zone_span_writelock(new_zone);
	end_pfn = new_zone->zone_start_pfn + new_zone->spanned_pages;
	if (!new_zone->zone_start_pfn && !new_zone->spanned_pages) {
		new_zone->zone_start_pfn = pfn;
		new_zone->spanned_pages = 1;
	} else if (pfn < new_zone->zone_start_pfn) {
		new_zone->spanned_pages += new_zone->zone_start_pfn - pfn;
		new_zone->zone_start_pfn = pfn;
	} else if (pfn >= end_pfn) {
		new_zone->spanned_pages  = pfn - new_zone->zone_start_pfn;
	}
	zone_span_writeunlock(new_zone);

	/* grow new node */
	end_pfn = new_node->node_start_pfn + new_node->node_spanned_pages;
	if (!new_node->node_start_pfn && !new_node->node_spanned_pages) {
		new_node->node_start_pfn = pfn;
		new_node->node_spanned_pages = 1;
	} else if (pfn < new_node->node_start_pfn) {
		new_node->node_spanned_pages += new_node->node_start_pfn - pfn;
		new_node->node_start_pfn = pfn;
	} else if (pfn >= end_pfn) {
		new_node->node_start_pfn = pfn - new_node->node_start_pfn;
	}
	pgdat_resize_unlock(new_node, &flags);
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
		/* XXX: do we need to change the zone in any cases? */
		/* XXX: fixup the old zone & new zone's present_pages & spanned_pages & zone_start_pfn */
		/* use span_seqlock */
		set_page_node(page, new_nid);

		dnuma_adjust_spanned_pages(pfn, get_zone(old_nid, zonenum), NODE_DATA(old_nid),
				get_zone(new_nid, zonenum), NODE_DATA(new_nid));

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
