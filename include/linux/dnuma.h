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
static inline void dnuma_make_changes(struct memlayout *new_ml)
{
	/* allocate a per node list of pages */

	/* for each node */
	/*	allocate a new pageblock_flags
	 *	lock the node
	 *		pull out pages that don't belong to it anymore, put them on the list.
	 *		give it a new pageblock_flags.
	 *		update spanned_pages, start pfn, free pages
	 *	unlock the node
	 *	free old pageblock_flags
	 */

	/* for each node */
	/*	add pages that are new to the node (and removed in the previous
	 *	iteration from other nodes) to it's free lists. (appropriate locking).
	 */
}

/* moves a page when it is being freed (and thus was not moved by
 * dnuma_move_free_pages()) */
static inline struct zone *dnuma_move_free_page_zone(struct page *page)
{
	/* XXX: handle pageblock_flags */
	enum zone_type zonenum = page_zonenum(page);
	int new_nid = memlayout_pfn_to_nid_no_pageflags(page_to_pfn(page));
	int old_nid = page_to_nid(page);

	if (new_nid == NUMA_NO_NODE)
		new_nid = old_nid;
	if (new_nid != old_nid) {
		unsigned long flags;
		/* XXX: do we need to change the zone in any cases? */
		/* XXX: fixup the old zone & new zone's present_pages & spanned_pages & zone_start_pfn */
		/* use span_seqlock */
		set_page_node(page, new_nid);

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
