#define pr_fmt(fmt) "dnuma: " fmt

#include <linux/dnuma.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "internal.h"

#if CONFIG_DNUMA_DEBUGFS
u64 dnuma_moved_page_ct;
DEFINE_SPINLOCK(dnuma_stats_lock);
#endif

/* Options wrt removing free pages from free lists:
 * - Isolation? - NO.
 *   - Can't determine which page ranges to isolate (memory belonging to this
 *     zone could be anywhwre)
 *   - too many things being isolated could trigger OOM.
 *   - Not too terrible if some stuff is allocated.
 * - lock zone & then grab a "bunch", lock new zone & give it a "bunch".
 * - Selecting pages to move
 *   - iterate over pfns
 *     - with dnuma, zones grow very large, could result in iterating over all
 *       pfns on the system.
 *     - pfns we examine could belong to a different zone, requiring skipping.
 *     - Fits with "destination" focus
 *   - iterate over free lists
 *     - pointer dereferences are not as fast as addition (pfn ++, etc).
 *     - all pages belong to the current zone, no skipping.
 *     - Fits with "source" focus
 * - Focus on the source zone or the destination zone (complexities are per
 *   zone operation, so don't really represent actual cost)
 *   - source
 *     - grab all pages in 1 source zone that need moving to any number of
 *       other zones.
 *     - repeat for each source zone. TC=O(n), MC=O(n).
 *   - destination
 *     - grab all pages from various sources which will be moved to 1
 *       destination.
 *     - repeat for each destination zone. TC=O(n), MC=O(1).
 *     - Potentially very high amount of locking/unlocking.
 *   - mixed
 *     - grab all pages in 1 source which need moving to 1 other zone.
 *     - repeat for all pairs of zones. TC = O(n^n), MC = O(1).
 *     - High time complexity. Does this actually represent a real slowdown?
 */

/* Issues due to pageflag_blocks attached to zones with Discontig Mem (&
 * Flatmem??).
 * - Need atomicity over the combination of commiting a new memlayout and
 *   removing the pages from free lists.
 */

/* - must be called under lock_memory_hotplug() */
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
	/* XXX: locking considerations:
	 *  - what can cause the hotplugging of a node? Do we just need to
	 *    lock_memory_hotplug()?
	 *  - pgdat_resize_lock()
	 *    - "Nests above zone->lock and zone->size_seqlock."
	 *  - zone_span_seq*() & zone_span_write*()
	 */

	struct rangemap_entry *rme;
	ml_for_each_range(new_ml, rme) {
		int range_nid = rme->nid;
		unsigned long pfn;
		LIST_HEAD(list);
		struct page *page, *page_tmp;

		for (pfn = rme->pfn_start; pfn <= rme->pfn_end; pfn++) {
			struct zone *zone;
			int page_nid, order;
			unsigned long flags, last_pfn, first_pfn;
			if (!pfn_valid(pfn))
				continue;

			page = pfn_to_page(pfn);

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

			/* FIXME: split pages when the range does not cover an
			 * entire order */

			/* gets page_order() assuming PageBuddy(page) */
			order = page_private(page);
			first_pfn = pfn & ~((1 << order) - 1);
			last_pfn  = pfn |  ((1 << order) - 1);
			if (WARN(pfn != first_pfn, "pfn %lu is not first_pfn %lu\n",
							pfn, first_pfn)) {
				pfn = last_pfn;
				goto skip_unlock;
			}

			if (WARN(last_pfn > rme->pfn_end,
					"last_pfn %lu goes beyond end of rme [ %lu - %lu ]\n",
					last_pfn, rme->pfn_start, rme->pfn_end)) {
				spin_unlock_irqrestore(&zone->lock, flags);
				break; /* done with this rme */
			}

			adjust_zone_present_pages(zone, -(1 << order));

			/* XXX: can we shrink spanned_pages & start_pfn without too much work? */

			/* zone free stats */
			zone->free_area[order].nr_free--;
			__mod_zone_page_state(zone, NR_FREE_PAGES,
					      -(1UL << order));

			list_move(&page->lru, &list);
skip_unlock:
			spin_unlock_irqrestore(&zone->lock, flags);
		}

		/* add grabbed pages to appropriate zone. */
		list_for_each_entry_safe(page, page_tmp, &list, lru) {
			struct zone *zone = get_zone(range_nid, page_zonenum(page));
			int order = page_private(page); /* gets page_order() assuming PageBuddy(page) */
			set_page_node(page, range_nid);

			adjust_zone_present_pages(zone, 1 << order);

			dnuma_prior_add_to_new_zone(page, order, zone, range_nid);

			/* FIXME hits VM_BUG in "static inline void __SetPageBuddy(struct page *page)" */
			return_pages_to_zone(page, order, zone);
		}
	}
}
