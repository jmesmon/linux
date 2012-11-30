#define pr_fmt(fmt) "dnuma: " fmt

#include <linux/dnuma.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#if CONFIG_DNUMA_DEBUGFS
u64 dnuma_moved_page_ct;
DEFINE_SPINLOCK(dnuma_stats_lock);
#endif

/* Options wrt removing free pages from free lists:
 * - Isolation? - NO.
 *   - Can't determine which page ranges to isolate (memory belonging to this zone could be anywhwre)
 *   - too many things being isolated could trigger OOM.
 *   - Not too terrible if some stuff is allocated.
 * - lock zone & then grab a "bunch", lock new zone & give it a "bunch".
 * - Selecting pages to move
 *   - iterate over pfns
 *     - with dnuma, zones grow very large, could result in iterating over all pfns on the system.
 *     - pfns we examine could belong to a different zone, requiring skipping.
 *     - Fits with "destination" focus
 *   - iterate over free lists
 *     - pointer dereferences are not as fast as addition (pfn ++, etc).
 *     - all pages belong to the current zone, no skipping.
 *     - Fits with "source" focus
 * - Focus on the source zone or the destination zone
 *   - source
 *     - grab all pages in 1 source zone that need moving to any number of other zones.
 *     - repeat for each source zone. TC=O(n), MC=O(n).
 *   - destination
 *     - grab all pages from various sources which will be moved to 1 destination.
 *     - repeat for each destination zone. TC=O(n), MC=O(1).
 *     - Potentially very high amount of locking/unlocking.
 *   - mixed
 *     - grab all pages in 1 source which need moving to 1 other zone.
 *     - repeat for all pairs of zones. TC = O(n^n), MC = O(1).
 *     - High time complexity. Does this actually represent a real slowdown?
 */

/* Issues due to pageflag_blocks attached to zones with Discontig Mem (& Flatmem??).
 * - Need atomicity over the combination of commiting a new memlayout and removing the pages from free lists.
 */

/* Do _not_ isolate the pfn range before calling this.
 * Based on __offline_isolated_pages()
 * - look @ move_freepages()
 * - also drain_all_pages. */

/* XXX: handling a pfn range that spans multiple zones & nodes whould be useful
 *      pass more lists? Or an array of lists?
 */
void remove_page_range_from_zone(unsigned long start_pfn, unsigned long end_pfn, struct list_head *head)
{
	struct page *page;
	struct zone *zone;
	int order, i;
	unsigned long pfn;
	unsigned long flags;
	/* find the first valid pfn */
	for (pfn = start_pfn; pfn < end_pfn; pfn++)
		if (pfn_valid(pfn))
			break;
	if (pfn == end_pfn)
		return;
	zone = page_zone(pfn_to_page(pfn));
	spin_lock_irqsave(&zone->lock, flags);
	pfn = start_pfn;
	while (pfn < end_pfn) {
		if (!pfn_valid(pfn)) {
			pfn++;
			continue;
		}
		page = pfn_to_page(pfn);
		//BUG_ON(page_count(page)); // ignore?
		//BUG_ON(!PageBuddy(page)); // should we just skip non-buddy pages?
		if (!PageBuddy(page)) {
			pfn++;
			continue;
		}
		//order = page_order(page); /* not exported from page_alloc.c */
		order = page_private(page); /* page_order() is just
					       page_private() with a
					       BUG_ON(!PageBuddy(page)) */
#ifdef CONFIG_DEBUG_VM
		printk(KERN_INFO "remove from free list %lx %d %lx\n",
		       pfn, 1 << order, end_pfn);
#endif
		list_del(&page->lru);

		/* XXX: does __ClearPageBuddy() and set_page_private(page, 0)
		 *      we probably want to not do the set_page_private(page, 0) bit.
		 *      Or we could add each page to the list individulay */
		//rmv_page_order(page); /* not exported from page_alloc.c */
		__ClearPageBuddy(page);
		/* XXX: also modify the total pages */
		/* XXX: fiddle with per-node stats? */
		zone->free_area[order].nr_free--;
		__mod_zone_page_state(zone, NR_FREE_PAGES,
				      - (1UL << order));
		/* XXX: Do we need to do this? Do we loose any info we need to
		 * restore the pages by doing this? */
		for (i = 0; i < (1 << order); i++)
			SetPageReserved((page+i));
		pfn += (1 << order);
	}
	spin_unlock_irqrestore(&zone->lock, flags);
}

/* requires must be called under lock_memory_hotplug() */
void dnuma_online_required_nodes(struct memlayout *new_ml)
{
	int nid;
	struct rangemap_entry *rme;
	ml_for_each_range(new_ml, rme) {
		nid = rme->nid;
		if (!node_online(nid))
			mem_online_node(nid);
	}
}

void dnuma_move_to_new_ml(struct memlayout *new_ml)
{
	/* allocate a per node list of pages */

	/* XXX: locking considerations:
	 *  - what can cause the hotplugging of a node? Do we just need to lock_memory_hotplug()?
	 *  - pgdat_resize_lock()
	 *    - "Nests above zone->lock and zone->size_seqlock."
	 *  - zone_span_seq*() & zone_span_write*() */

	struct rangemap_entry *rme;
	ml_for_each_range(new_ml, rme) {
		int range_nid = rme->nid;
		unsigned long pfn;
		LIST_HEAD(list);

		ml_for_each_pfn_in_range(rme, pfn) {
			struct page *page = pfn_to_page(pfn);
			struct zone *zone;
			int page_nid, order;
			unsigned long flags;

			/* Currently allocated, will be fixed up when freed. */
			if (!PageBuddy(page))
				continue;

			page_nid = page_to_nid(page);
			/* Page is already in the "right" zone */
			if (page_nid == range_nid)
				continue;

			zone = page_zone(page);
			spin_lock_irqsave(&zone->lock, flags);

			/* Someone allocated it since we last checked. It will
			 * be fixed up when it is freed */
			if (!PageBuddy(page))
				goto skip_unlock;

			/* It has already migrated "somewhere" */
			if (page_zone(page) != zone)
				goto skip_unlock;
			order = page_private(page); /* gets page_order() assuming PageBuddy(page) */

			/* FIXME: locking? */
			zone->present_pages --;

			/* zone free stats */
			zone->free_area[order].nr_free--;
			__mod_zone_page_state(zone, NR_FREE_PAGES,
					      - (1UL << order));

			migratetype = get_freepage_migratetype(page);

			list_move(&page->lru,
					&zone->free_area[order].free_list[migratetype]);
			set_freepage_migratetype(page, migratetype);
			/* TODO: lookup lruvec
			 * 	remove from that list
			 * 	add to @list
			 */

skip_unlock:
			spin_unlock_irqsave(&zone->lock, flags);
		}

		/* TODO: add grabbed pages to appropriate zone. */
	}
}


