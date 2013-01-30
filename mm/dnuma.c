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

/* XXX: "present pages" is guarded by lock_memory_hotplug(), not the spanlock.
 * Need to change all users. */
void adjust_zone_present_pages(struct zone *zone, long delta)
{
	unsigned long flags;
	pgdat_resize_lock(zone->zone_pgdat, &flags);
	zone_span_writelock(zone);

	zone->managed_pages += delta;
	zone->present_pages += delta;
	zone->zone_pgdat->node_present_pages += delta;

	zone_span_writeunlock(zone);
	pgdat_resize_unlock(zone->zone_pgdat, &flags);
}

/* - must be called under lock_memory_hotplug() */
/* TODO: avoid iterating over all PFNs. */
void dnuma_online_required_nodes_and_zones(struct memlayout *new_ml)
{
	struct rangemap_entry *rme;
	ml_for_each_range(new_ml, rme) {
		unsigned long pfn;
		int nid = rme->nid;

		if (!node_online(nid)) {
			pr_debug("onlining node %d\n", nid);
			__mem_online_node(nid);
		}

		/* Determine the zones required */
		for (pfn = rme->pfn_start; pfn <= rme->pfn_end; pfn++) {
			struct zone *zone;
			if (!pfn_valid(pfn))
				continue;

			zone = nid_zone(nid, page_zonenum(pfn_to_page(pfn)));
			WARN_ON(ensure_zone_is_initialized(zone, 0, 0));
		}
	}
}

/*
 * Cannot be folded into dnuma_move_unallocated_pages() because unmarked pages
 * could be freed back into the zone as dnuma_move_unallocated_pages() was in
 * the process of iterating over it.
 */
void dnuma_mark_page_range(struct memlayout *new_ml)
{
	struct rangemap_entry *rme;
	ml_for_each_range(new_ml, rme) {
		unsigned long pfn;
		for (pfn = rme->pfn_start; pfn <= rme->pfn_end; pfn++) {
			if (!pfn_valid(pfn))
				continue;
			/* FIXME: should we be skipping compound / buddied pages? */
			/* FIXME: if PageReserved(), can we just poke the nid directly? */
			SetPageLookupNode(pfn_to_page(pfn));
		}
	}
}

#if 0
static void node_states_set_node(int node, struct memory_notify *arg)
{
	if (arg->status_change_nid_normal >= 0)
		node_set_state(node, N_NORMAL_MEMORY);

	if (arg->status_change_nid_high >= 0)
		node_set_state(node, N_HIGH_MEMORY);

	node_set_state(node, N_MEMORY);
}
#endif

void dnuma_post_free_to_new_zone(struct page *page, int order)
{
	/* LOCKS. BLAH */
	adjust_zone_present_pages(page_zone(page), (1 << order));
}

static void dnuma_prior_return_to_new_zone(struct page *page, int order,
					   struct zone *dest_zone,
					   int dest_nid)
{
	int i;
	unsigned long pfn = page_to_pfn(page);

	grow_pgdat_and_zone(dest_zone, pfn, pfn + (1 << order));

	for (i = 0; i < 1 << order; i++)
		set_page_node(&page[i], dest_nid);
}

static void clear_lookup_node(struct page *page, int order)
{
	int i;
	for (i = 0; i < 1 << order; i++)
		ClearPageLookupNode(&page[i]);
}

/* Does not assume it is called with any locking (but can be called with zone
 * locks held, if needed) */
void dnuma_prior_free_to_new_zone(struct page *page, int order,
				  struct zone *dest_zone,
				  int dest_nid)
{
	struct zone *curr_zone = page_zone(page);

	/* XXX: Fiddle with 1st zone's locks */
	adjust_zone_present_pages(curr_zone, -(1 << order));

	/* XXX: fiddles with 2nd zone's locks */
	dnuma_prior_return_to_new_zone(page, order, dest_zone, dest_nid);
}

/* must be called with zone->lock held */
static void remove_free_page_from_zone(struct zone *zone, struct page *page, int order)
{
	/* zone free stats */
	zone->free_area[order].nr_free--;
	__mod_zone_page_state(zone, NR_FREE_PAGES,
			      -(1UL << order));
	adjust_zone_present_pages(zone, -(1 << order));

	list_del(&page->lru);

	/* avoid a VM_BUG in __free_page_ok */
	VM_BUG_ON(!PageBuddy(page));
	__ClearPageBuddy(page);
}

/*
 * __ref is to allow (__meminit) zone_pcp_update(), which we will have because
 * DYNAMIC_NUMA depends on MEMORY_HOTPLUG.
 */
void __ref dnuma_move_unallocated_pages(struct memlayout *new_ml)
{
	/* XXX: locking considerations:
	 *  - what can cause the hotplugging of a node? Do we just need to
	 *    lock_memory_hotplug()?
	 */

	/* migrate types?
	 * ISOLATION?
	 */
	struct rangemap_entry *rme;
	ml_for_each_range(new_ml, rme) {
		int range_nid = rme->nid;
		unsigned long pfn;
		struct page *page;

		for (pfn = rme->pfn_start; pfn <= rme->pfn_end; pfn++) {
			struct zone *zone, *dest_zone;
			int page_nid, order;
			unsigned long flags, last_pfn, first_pfn;
			bool need_zonelists_rebuild = false;
			if (!pfn_valid(pfn))
				continue;

			page = pfn_to_page(pfn);

			/* XXX: what can "unreserve" pages? */
#if 0
			if (PageReserved(page)) {
				fiddle with the node;
				adjust spanned_pages & present_pages & start_pfn.
			}
#endif

			/* Currently allocated, will be fixed up when freed. */
			if (!PageBuddy(page))
				goto skip_valid;

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

			dest_zone = nid_zone(range_nid, page_zonenum(page));
			VM_BUG_ON(!zone_is_initialized(dest_zone));

			if (zone_is_empty(dest_zone))
				need_zonelists_rebuild = true;

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

			clear_lookup_node(page, order);
			remove_free_page_from_zone(zone, page, order);
			/* XXX: can we shrink spanned_pages & start_pfn without too much work?
			 *  - not crutial because having a
			 *    larger-than-necessary span simply means that more
			 *    PFNs are iterated over.
			 *  - would be nice to be able to do this to cut down
			 *    on overhead caused by PFN iterators.
			 */
			spin_unlock_irqrestore(&zone->lock, flags);

			/* Add page to new zone */
			dnuma_prior_return_to_new_zone(page, order, dest_zone, range_nid);
			return_pages_to_zone(page, order, dest_zone);
			dnuma_post_free_to_new_zone(page, order);

			/* XXX: fixme, there are other states that need fixing up */
			if (!node_state(range_nid, N_MEMORY))
				node_set_state(range_nid, N_MEMORY);

			if (need_zonelists_rebuild)
				/* XXX: also does stop_machine */
				build_all_zonelists(NULL, dest_zone);
			else
				/* FIXME: does stop_machine after EVERY SINGLE PAGE */
				zone_pcp_update(dest_zone);

			pfn = last_pfn;
			continue;
skip_unlock:
			spin_unlock_irqrestore(&zone->lock, flags);
skip_valid:
			;
		}
	}
}
