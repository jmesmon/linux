#define pr_fmt(fmt) "dnuma: " fmt

#include <linux/dnuma.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/atomic.h>

#include "internal.h"

#if CONFIG_DNUMA_DEBUGFS
atomic64_t dnuma_moved_page_ct;
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

void adjust_zone_present_pages(struct zone *zone, long delta)
{
	unsigned long flags;
	pgdat_resize_lock(zone->zone_pgdat, &flags);
	zone_span_writelock(zone);

	zone->present_pages += delta;
	zone->zone_pgdat->node_present_pages += delta;

	zone_span_writeunlock(zone);
	pgdat_resize_unlock(zone->zone_pgdat, &flags);
}

/* - must be called under lock_memory_hotplug() */
void dnuma_online_required_nodes(struct memlayout *new_ml)
{
	int nid;
	struct rangemap_entry *rme;
	ml_for_each_range(new_ml, rme) {
		nid = rme->nid;
		if (!node_online(nid)) {
			pr_debug("onlining node %d\n", nid);
			mem_online_node(nid);
		}
	}
}

/* Always called after dnuma_move_to_new_ml() & the assigning of the new
 * memlayout */
/* FIXME: folding this into dnuma_move_to_new_ml() could allow us to avoid
 * re-processing the free pages which are currently transplanted within
 * dnuma_move_to_new_ml(). */
void dnuma_mark_page_range(struct memlayout *new_ml)
{
	struct rangemap_entry *rme;
	ml_for_each_range(new_ml, rme) {
		unsigned long pfn;
		for (pfn = rme->pfn_start; pfn <= rme->pfn_end; pfn++) {
			struct page *page = pfn_to_page(pfn);
			/* FIXME: should we be skipping compound / buddied pages? */
			SetPageLookupNode(page);
		}
	}
}

/* Does memory allocation in ensure_zone_is_initialized(), must be called with
 * is_atomic=false (ie: no spinlocks, no rcu).
 */
void dnuma_move_to_new_ml(struct memlayout *new_ml)
{
	/* XXX: locking considerations:
	 *  - what can cause the hotplugging of a node? Do we just need to
	 *    lock_memory_hotplug()?
	 */

	/* migrate types?
	 * ISOLATION?
	 */
	lock_memory_hotplug();
	struct rangemap_entry *rme;
	ml_for_each_range(new_ml, rme) {
		int range_nid = rme->nid;
		unsigned long pfn;
		struct page *page;

		for (pfn = rme->pfn_start; pfn <= rme->pfn_end; pfn++) {
			struct zone *zone, *dest_zone;
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
retry_page:
			spin_lock_irqsave(&zone->lock, flags);

			/* Someone allocated it since we last checked. It will
			 * be fixed up when it is freed */
			if (!PageBuddy(page))
				goto skip_unlock;

			/* It has already migrated "somewhere" */
			if (page_zone(page) != zone)
				goto skip_unlock;

			/* TODO: online required zones outside of atomic (they need memory allocations) */
			dest_zone = nid_zone(range_nid, page_zonenum(page));
			if (!zone_is_initialized(dest_zone)) {
				/* TODO: transplanting this page. */
				spin_unlock_irqrestore(&zone->lock, flags);
				/* XXX: locking on this? lock_memory_hotplug()? lock(dest_zone->lock)? */
				ensure_zone_is_initialized(dest_zone, 0, 0);
				goto retry_page;
			}

			/* FIXME: split pages when the range does not cover an
			 * entire order */

			order = page_order(page);
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

			list_del(&page->lru);

			spin_unlock_irqrestore(&zone->lock, flags);

			/* Add page to new zone */
			dnuma_prior_add_to_new_zone(page, order, dest_zone, range_nid);
			return_pages_to_zone(page, order, dest_zone);
			adjust_zone_present_pages(dest_zone, 1 << order);

			continue;
skip_unlock:
			spin_unlock_irqrestore(&zone->lock, flags);
		}
	}

	unlock_memory_hotplug();
}
