#define pr_fmt(fmt) "dnuma: " fmt

#include <linux/atomic.h>
#include <linux/bootmem.h>
#include <linux/dnuma.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "internal.h"

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

			/* XXX: we aren't really onlining memory, but some code
			 * uses memory online notifications to tell if new
			 * nodes have been created.
			 *
			 * Also note that the notifiers expect to be able to do
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
				 *        "normal" memory in it, can we check
				 *        for this?
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
	dnuma_prior_return_to_new_zone(page, order, dest_zone, dest_nid);
}

/* must be called with zone->lock held and memlayout's update_lock held */
static void remove_free_pages_from_zone(struct zone *zone, struct page *page,
					int order)
{
	/* zone free stats */
	zone->free_area[order].nr_free--;
	__mod_zone_page_state(zone, NR_FREE_PAGES, -(1UL << order));

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
static void __ref add_free_page_to_node(int dest_nid, struct page *page,
					int order)
{
	bool need_zonelists_rebuild = false;
	struct zone *dest_zone = nid_zone(dest_nid, page_zonenum(page));
	VM_BUG_ON(!zone_is_initialized(dest_zone));

	if (zone_is_empty(dest_zone))
		need_zonelists_rebuild = true;

	/* Add page to new zone */
	dnuma_prior_return_to_new_zone(page, order, dest_zone, dest_nid);
	return_pages_to_zone(page, order, dest_zone);
	dnuma_post_free_to_new_zone(order);

	/* XXX: fixme, there are other states that need fixing up */
	if (!node_state(dest_nid, N_MEMORY))
		node_set_state(dest_nid, N_MEMORY);

	if (need_zonelists_rebuild) {
		/* XXX: also does stop_machine() */
		zone_pcp_reset(dest_zone);
		/* XXX: why is this locking actually needed? */
		mutex_lock(&zonelists_mutex);
#if 0
		/* assumes that zone is unused */
		setup_zone_pageset(dest_zone);
		build_all_zonelists(NULL, NULL);
#else
		build_all_zonelists(NULL, dest_zone);
#endif
		mutex_unlock(&zonelists_mutex);
	}
}

static struct rangemap_entry *add_split_pages_to_zones(
		struct rangemap_entry *first_rme,
		struct page *page, int order)
{
	int i;
	struct rangemap_entry *rme = first_rme;
	/*
	 * We avoid doing any hard work to try to split the pages optimally
	 * here because the page allocator splits them into 0-order pages
	 * anyway.
	 *
	 * XXX: All of the checks for NULL rmes and the nid conditional are to
	 * work around memlayouts potentially not covering all valid memory.
	 */
	for (i = 0; i < (1 << order); i++) {
		unsigned long pfn = page_to_pfn(page);
		int nid;
		while (rme && pfn > rme->pfn_end)
			rme = rme_next(rme);

		if (rme && pfn >= rme->pfn_start)
			nid = rme->nid;
		else
			nid = page_to_nid(page + i);

		add_free_page_to_node(nid, page + i, 0);
	}

	return rme;
}

#define _page_count_idx(managed, nid, zone_num) \
	(managed + 2 * (zone_num + MAX_NR_ZONES * (nid)))
#define page_count_idx(nid, zone_num) _page_count_idx(0, nid, zone_num)

/* Because we hold lock_memory_hotplug(), we assume that no else will be
 * changing present_pages and managed_pages.
 */
static void update_page_counts(struct memlayout *new_ml)
{
	/* Perform a combined iteration of pgdat+zones and memlayout.
	 * - memlayouts are ordered, their lookup from pfn is slow, and they
	 *   might have holes over valid pfns.
	 * - pgdat+zones are unordered, have O(1) lookups, and don't have holes
	 *   over valid pfns.
	 */
	struct rangemap_entry *rme;
	unsigned long pfn = 0;
	unsigned long *counts = kzalloc(2 * nr_node_ids * MAX_NR_ZONES *
						sizeof(*counts),
					GFP_KERNEL);
	if (WARN_ON(!counts))
		return;
	rme = rme_first(new_ml);
	for (pfn = 0; pfn < max_pfn; pfn++) {
		int nid;
		struct page *page;
		size_t idx;

		if (!pfn_valid(pfn))
			continue;

		page = pfn_to_page(pfn);
recheck_rme:
		if (!rme || pfn < rme->pfn_start) {
			/* We are before the start of the current rme, or we
			 * are past the last rme, fallback on pgdat+zone+page
			 * data. */
			nid = page_to_nid(page);
			pr_debug("FALLBACK: pfn %05lx, put in node %d. current rme "RME_FMT"\n",
					pfn, nid, RME_EXP(rme));
		} else if (pfn > rme->pfn_end) {
			rme = rme_next(rme);
			goto recheck_rme;
		} else {
			nid = rme->nid;
		}

		idx = page_count_idx(nid, page_zonenum(page));
		/* XXX: what happens if pages become
		   reserved/unreserved during this
		   process? */
		if (!PageReserved(page))
			counts[idx]++; /* managed_pages */
		counts[idx + 1]++;     /* present_pages */
	}

	{
		int nid;
		for (nid = 0; nid < nr_node_ids; nid++) {
			unsigned long nid_present = 0;
			int zone_num;
			pg_data_t *node = NODE_DATA(nid);
			if (!node)
				continue;
			for (zone_num = 0; zone_num < node->nr_zones;
					zone_num++) {
				struct zone *zone = &node->node_zones[zone_num];
				size_t idx = page_count_idx(nid, zone_num);
				pr_debug("nid %d zone %d mp=%lu pp=%lu -> mp=%lu pp=%lu\n",
						nid, zone_num,
						zone->managed_pages,
						zone->present_pages,
						counts[idx], counts[idx+1]);
				zone->managed_pages = counts[idx];
				zone->present_pages = counts[idx + 1];
				nid_present += zone->present_pages;

				/*
				 * recalculate pcp ->batch & ->high using
				 * zone->managed_pages
				 */
				zone_pcp_update(zone);
			}

			pr_debug(" node %d zone * present_pages %lu to %lu\n",
					node->node_id, node->node_present_pages,
					nid_present);
			node->node_present_pages = nid_present;
		}
	}

	kfree(counts);
}

void __ref dnuma_move_free_pages(struct memlayout *new_ml)
{
	struct rangemap_entry *rme;

	update_page_counts(new_ml);
	init_per_zone_wmark_min();

	/* FIXME: how does this removal of pages from a zone interact with
	 * migrate types? ISOLATION? */
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
				/* TODO: adjust spanned_pages & present_pages &
				 * start_pfn. */
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
				VM_BUG_ON(zone != nid_zone(range_nid,
							page_zonenum(page)));
				goto skip_unlock;
			}

			order = page_order(page);
			first_pfn = pfn & ~((1 << order) - 1);
			last_pfn  = pfn |  ((1 << order) - 1);
			if (WARN(pfn != first_pfn,
					"pfn %05lx is not first_pfn %05lx\n",
					pfn, first_pfn)) {
				pfn = last_pfn;
				goto skip_unlock;
			}

			if (last_pfn > rme->pfn_end) {
				/*
				 * this higher order page doesn't fit into the
				 * current range even though it starts there.
				 */
				pr_warn("order-%02d page (pfn %05lx-%05lx) extends beyond end of rme "RME_FMT"\n",
						order, first_pfn, last_pfn,
						RME_EXP(rme));

				remove_free_pages_from_zone(zone, page, order);
				spin_unlock_irqrestore(&zone->lock, flags);

				rme = add_split_pages_to_zones(rme, page,
						order);
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
