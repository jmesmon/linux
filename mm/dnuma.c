#define pr_fmt(fmt) "dnuma: " fmt

#include <linux/dnuma.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/memory.h>

#include "internal.h"
#include "memlayout-debugfs.h"

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
			pr_info("onlining node %d [start]\n", nid);

			/* Consult hotadd_new_pgdat() */
			__mem_online_node(nid);

			/* XXX: somewhere in here do a memory online notify: we
			 * aren't really onlining memory, but some code uses
			 * memory online notifications to tell if new nodes
			 * have been created.
			 *
			 * Also note that the notifyers expect to be able to do
			 * allocations, ie we must allow for might_sleep() */
			{
				int ret;

				/* memory_notify() expects:
				 *	- to add pages at the same time
				 *	- to add zones at the same time
				 * We can do neither of these things.
				 *
				 * XXX: - slab uses .status_change_nid
				 *      - slub uses .status_change_nid_normal
				 * FIXME: for slub, we may not be placing any
				 *        "normal" memory in it, can we check for
				 *        this?
				 */
				struct memory_notify arg = {
					.status_change_nid = nid,
					.status_change_nid_normal = nid,
				};

				ret = memory_notify(MEM_GOING_ONLINE, &arg);
				ret = notifier_to_errno(ret);
				if (WARN_ON(ret)) {
					/* XXX: other stuff will bug out if we
					 * keep going, need to actually cancel
					 * memlayout changes
					 */
					memory_notify(MEM_CANCEL_ONLINE, &arg);
				}
			}

			pr_info("onlining node %d [complete]\n", nid);
		}

		/* Determine the zones required */
		for (pfn = rme->pfn_start; pfn <= rme->pfn_end; pfn++) {
			struct zone *zone;
			if (!pfn_valid(pfn))
				continue;

			zone = nid_zone(nid, page_zonenum(pfn_to_page(pfn)));
			/* XXX: we (dnuma paths) can handle this (there will
			 * just be quite a few WARNS in the logs), but if we
			 * are indicating error above, should we bail out here
			 * as well? */
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
			/* FIXME: should we be skipping compound / buddied
			 *        pages? */
			/* FIXME: if PageReserved(), can we just poke the nid
			 *        directly? Should we? */
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
	adjust_zone_present_pages(page_zone(page), (1 << order));
	ml_stat_count_moved_pages(order);
}

static void dnuma_prior_return_to_new_zone(struct page *page, int order,
					   struct zone *dest_zone,
					   int dest_nid)
{
	int i;
	unsigned long pfn = page_to_pfn(page);

	grow_pgdat_and_zone(dest_zone, pfn, pfn + (1UL << order));

	for (i = 0; i < 1UL << order; i++)
		set_page_node(&page[i], dest_nid);
}

static void clear_lookup_node(struct page *page, int order)
{
	int i;
	for (i = 0; i < 1UL << order; i++)
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
	adjust_zone_present_pages(curr_zone, -(1UL << order));

	/* XXX: fiddles with 2nd zone's locks */
	dnuma_prior_return_to_new_zone(page, order, dest_zone, dest_nid);
}

/* must be called with zone->lock held and memlayout's update_lock held */
static void remove_free_pages_from_zone(struct zone *zone, struct page *page, int order)
{
	/* zone free stats */
	zone->free_area[order].nr_free--;
	__mod_zone_page_state(zone, NR_FREE_PAGES, -(1UL << order));
	adjust_zone_present_pages(zone, -(1UL << order));

	list_del(&page->lru);
	__ClearPageBuddy(page);

	/* Allowed because we hold the memlayout update_lock. */
	clear_lookup_node(page, order);

	/* XXX: can we shrink spanned_pages & start_pfn without too much work?
	 *  - not crutial because having a
	 *    larger-than-necessary span simply means that more
	 *    PFNs are iterated over.
	 *  - would be nice to be able to do this to cut down
	 *    on overhead caused by PFN iterators.
	 */
}

/*
 * __ref is to allow (__meminit) zone_pcp_update(), which we will have because
 * DYNAMIC_NUMA depends on MEMORY_HOTPLUG (and all the MEMORY_HOTPLUG comments
 * indicate __meminit is allowed when they are enabled).
 */
static void __ref add_free_page_to_node(int dest_nid, struct page *page, int order)
{
	bool need_zonelists_rebuild = false;
	struct zone *dest_zone = nid_zone(dest_nid, page_zonenum(page));
	VM_BUG_ON(!zone_is_initialized(dest_zone));

	if (zone_is_empty(dest_zone))
		need_zonelists_rebuild = true;

	/* Add page to new zone */
	dnuma_prior_return_to_new_zone(page, order, dest_zone, dest_nid);
	return_pages_to_zone(page, order, dest_zone);
	dnuma_post_free_to_new_zone(page, order);

	/* XXX: fixme, there are other states that need fixing up */
	if (!node_state(dest_nid, N_MEMORY))
		node_set_state(dest_nid, N_MEMORY);

	if (need_zonelists_rebuild) {
		/* XXX: also does stop_machine() */
		//zone_pcp_reset(zone);
		/* XXX: why is this locking actually needed? */
		mutex_lock(&zonelists_mutex);
		//build_all_zonelists(NULL, NULL);
		build_all_zonelists(NULL, dest_zone);
		mutex_unlock(&zonelists_mutex);
	} else
		/* FIXME: does stop_machine() after EVERY SINGLE PAGE */
		/* XXX: this is probably wrong. What does "update" actually
		 * indicate in zone_pcp terms? */
		zone_pcp_update(dest_zone);
}

static struct rangemap_entry *add_split_pages_to_zones(
		struct rangemap_entry *first_rme,
		struct page *page, int order)
{
	int i;
	struct rangemap_entry *rme = first_rme;
	for (i = 0; i < (1 << order); i++) {
		unsigned long pfn = page_to_pfn(page);
		while (pfn > rme->pfn_end) {
			rme = rme_next(rme);
		}

		add_free_page_to_node(rme->nid, page + i, 0);
	}

	return rme;
}

void dnuma_move_free_pages(struct memlayout *new_ml)
{
	/* FIXME: how does this removal of pages from a zone interact with
	 * migrate types? ISOLATION? */
	struct rangemap_entry *rme;
	ml_for_each_range(new_ml, rme) {
		unsigned long pfn = rme->pfn_start;
		int range_nid;
		struct page *page;
new_rme:
		range_nid = rme->nid;

		for (; pfn <= rme->pfn_end; pfn++) {
			struct zone *zone;
			int page_nid, order;
			unsigned long flags, last_pfn, first_pfn;
			if (!pfn_valid(pfn))
				continue;

			page = pfn_to_page(pfn);
#if 0
			/* XXX: can we ensure this is safe? Pages marked
			 * reserved could be freed into the page allocator if
			 * they mark memory areas that were allocated via
			 * earlier allocators. */
			if (PageReserved(page)) {
				set_page_node(page, range_nid);
				/* TODO: adjust spanned_pages & present_pages & start_pfn. */
			}
#endif

			/* Currently allocated, will be fixed up when freed. */
			if (!PageBuddy(page))
				continue;

			page_nid = page_to_nid(page);
			if (page_nid == range_nid)
				continue;

			zone = page_zone(page);
			spin_lock_irqsave(&zone->lock, flags);

			/* Someone allocated it since we last checked. It will
			 * be fixed up when it is freed */
			if (!PageBuddy(page))
				goto skip_unlock;

			/* It has already been transplanted "somewhere",
			 * somewhere should be the proper zone. */
			if (page_zone(page) != zone) {
				VM_BUG_ON(zone != nid_zone(range_nid, page_zonenum(page)));
				goto skip_unlock;
			}

			order = page_order(page);
			first_pfn = pfn & ~((1 << order) - 1);
			last_pfn  = pfn |  ((1 << order) - 1);
			if (WARN(pfn != first_pfn, "pfn %05lx is not first_pfn %05lx\n",
							pfn, first_pfn)) {
				pfn = last_pfn;
				goto skip_unlock;
			}

			if (last_pfn > rme->pfn_end) {
				/* this higher order page doesn't fit into the
				 * current range even though it starts there.
				 */
				pr_warn("high-order page from pfn %05lx to %05lx extends beyond end of rme {%05lx - %05lx}:%d\n",
						first_pfn, last_pfn,
						rme->pfn_start, rme->pfn_end,
						rme->nid);

				remove_free_pages_from_zone(zone, page, order);
				spin_unlock_irqrestore(&zone->lock, flags);

				rme = add_split_pages_to_zones(rme, page, order);
				pfn = last_pfn + 1;
				goto new_rme;
			}

			remove_free_pages_from_zone(zone, page, order);
			spin_unlock_irqrestore(&zone->lock, flags);

			add_free_page_to_node(range_nid, page, order);
			pfn = last_pfn;
			continue;
skip_unlock:
			spin_unlock_irqrestore(&zone->lock, flags);
		}
	}
}
