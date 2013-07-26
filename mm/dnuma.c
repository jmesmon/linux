#define pr_fmt(fmt) "dnuma: " fmt

#include <linux/atomic.h>
#include <linux/bootmem.h>
#include <linux/dnuma.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/swap.h> /* kswapd_run */
#include <linux/types.h>
#include <linux/writeback.h> /* writeback_set_ratelimit */

#include "internal.h"
#include "memlayout-debugfs.h"

/*
 * used as the slowpath for
 *	int dnuma_page_needs_move(struct page *page);
 */
int dnuma_page_needs_move_lookup(struct page *page)
{
	int new_nid, old_nid;
	unsigned long pfn = page_to_pfn(page);

	new_nid = memlayout_pfn_to_nid_if_active(pfn);
	if (WARN_ON(new_nid == NUMA_NO_NODE))
		return NUMA_NO_NODE;

	old_nid = page_to_nid(page);

	if (new_nid == NUMA_NO_NODE) {
		pr_alert("dnuma: pfn %05lx has moved from node %d to a non-memlayout range.\n",
				pfn, old_nid);
		return NUMA_NO_NODE;
	}

	if (new_nid == old_nid)
		return NUMA_NO_NODE;

	if (WARN_ON(!zone_is_initialized(
			nid_zone(new_nid, page_zonenum(page)))))
		return NUMA_NO_NODE;

	return new_nid;
}

#ifdef CONFIG_DNUMA_MANAGED_PAGE_UPDATE_DELAY
void dnuma_page_being_allocated(struct zone *zone, struct page *page, int order)
{
	struct memlayout *ml;
	int nid;
	struct zone *future_zone;

	rcu_read_lock();
	ml = memlayout_rcu_deref_if_active();
	if (!ml)
		goto out;

	nid = _memlayout_pfn_to_nid(ml, page_to_pfn(page));
	if (nid == NUMA_NO_NODE)
		goto out;

	future_zone = nid_zone(nid, zone_idx(zone));

	zone_adjust_managed_page_count(zone, -(1 << order));
	zone_adjust_managed_page_count(future_zone, 1 << order);

	update_per_zone_wmark_min();

	ml_stat_add(MLSTAT_FUTURE_ZONE_FIXUP, ml, nid, order);
out:
	rcu_read_unlock();
}
#else
void dnuma_page_being_allocated(struct zone *zone, struct page *page, int order)
{}
#endif

static void lookup_node_clear_pfn(unsigned long pfn)
{
	unsigned long first_pfn_in_sec = SECTION_ALIGN_DOWN(pfn);
	struct mem_section *ms = __pfn_to_section(pfn);
	if (!ms->lookup_node_mark)
		return;

	clear_bit(pfn - first_pfn_in_sec, ms->lookup_node_mark);
}

/*
 * for use while the memlayout update lock is held
 *
 * TODO: by using our knowledge of where section boundaries are located, we
 * could decrease the checks for lookup_node_mark being allocated.
 */
static void lookup_node_clear_order(struct page *page, int order)
{
	unsigned long base_pfn = page_to_pfn(page);
	unsigned long pfn;
	for (pfn = base_pfn; pfn < base_pfn + (1UL << order); pfn++)
		lookup_node_clear_pfn(pfn);
}

/*
 * Be very careful about holding zonelocks while calling this function:
 * essentially, don't hold them.
 */
static void lookup_node_mark_pfn(unsigned long pfn)
{
	unsigned long first_pfn_in_sec = SECTION_ALIGN_DOWN(pfn);
	struct mem_section *ms = __pfn_to_section(pfn);

	if (!ms->lookup_node_mark) {
		ms->lookup_node_mark =
			kzalloc(sizeof(*ms->lookup_node_mark) *
				 BITS_TO_LONGS(PAGES_PER_SECTION), GFP_KERNEL);

		if (!ms->lookup_node_mark) {
			pr_warn("node mark allocation failed, some memory will not be transplanted.\n");
			return;
		}
	}

	set_bit(pfn - first_pfn_in_sec, ms->lookup_node_mark);
}

/*
 * must be called under lock_memory_hotplug()
 */
int dnuma_online_page_range(unsigned long start_pfn, unsigned long end_pfn,
		struct rangemap_entry *rme)
{
	unsigned long pfn;
	int nid = rme->nid;

	if (!node_online(nid)) {
		__mem_online_node(nid);

		/*
		 * we aren't really onlining memory, but some code
		 * uses memory online notifications to tell if new
		 * nodes have been created.
		 *
		 * Also note that the notifiers expect to be able to do
		 * allocations, ie we must allow for might_sleep()
		 */
		{
			int ret;

			/*
			 * memory_notify() expects:
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
				/* FIXME: other stuff will bug out if we
				 * keep going, need to actually cancel
				 * memlayout changes
				 */
				memory_notify(MEM_CANCEL_ONLINE, &arg);
				return -ENOMEM;
			}
		}
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
		if (WARN_ON(ensure_zone_is_initialized(zone, 0, 0)))
			return -ENOMEM;
	}

	return 0;
}

static struct rangemap_entry *advance_rme(struct rangemap_entry *rme,
		unsigned long start_pfn)
{
	while (rme && rme->pfn_end < start_pfn)
		rme = rme_next(rme);

	return rme;
}

#ifdef CONFIG_DNUMA_MANAGED_PAGE_UPDATE_DELAY
/*
 * given a zone, calculate
 * zone->managed_pages += <non-free pages that will be transplanted _to_ zone on free>
 *			- <non-free pages that will be transplanted _away_ from zone on free>
 *
 * Essentially: update a given @zone's managed_pages due to in-flight
 * (allocated) pages.
 *
 * This is split from update_present_page_counts() because we need to hold a
 * lock on the examined zone for the entirety of this function.
 *
 * TODO:
 *	- use zone spans to limit pfn scanning
 *
 * Issues:
 *	opens a hole where pages in this zone's freelists could be allocated
 *	(from other zones) between the time where we set this value and where
 *	the pages are transplanted between freelists, causing this value to be
 *	incorrect. To correct, we need to adjust managed pages when a page is
 *	allocated from a zone but will be transplanted.
 *
 *	No way to avoid, hooking the allocation path required.
 */
static void zone_update_managed_pages(struct zone *zone, struct memlayout *ml)
{
	unsigned long flags;
	//unsigned long seq;
	struct rangemap_entry *rme;
	unsigned long to = 0, from = 0;
	spin_lock_irqsave(&zone->lock, flags);
	ml_for_each_range(ml, rme) {
		unsigned long pfn;
		for (pfn = rme->pfn_start; pfn <= rme->pfn_end; pfn++) {
			struct page *page;
			struct zone *dest_zone, *curr_zone;
			if (!pfn_valid(pfn))
				continue;

			page = pfn_to_page(pfn);
			if (PageBuddy(page))
				continue;

			dest_zone = nid_zone(rme->nid, page_zonenum(page));
			curr_zone = page_zone(page);

			if (dest_zone == zone && !(curr_zone == zone))
				to++;
			else if (!(dest_zone == zone) && (curr_zone == zone))
				from++;
		}
	}
#if 0
	do {
		seq = zone_span_seqbegin(zone);
	} while (zone_span_seqretry(zone, seq));
#endif
	spin_unlock_irqrestore(&zone->lock, flags);

	pr_info("adjust_managed_page_count: Node %d %s: +%ld-%ld = %ld\n", zone->node, zone->name, to, from, to-from);
	zone_adjust_managed_page_count(zone, to - from);
}
#endif

/*
 * Note that this iteration assumes that memlayouts are contiguous, and have
 * the same minimal and maximal pfn.
 * 
 * Iterate over a pair of memlayouts
 */
#define ml_pair_for_each(ml_new, ml_old, rme_new, rme_old, \
		range_start_pfn, range_end_pfn)	\
	for ((rme_new) = rme_first(ml_new), \
		(rme_old) = rme_first(ml_old),\
		range_start_pfn = min((rme_new)->pfn_start, (rme_old)->pfn_start); \
		\
		((rme_old) && (rme_new)) ? \
		(range_end_pfn = min((rme_new)->pfn_end, (rme_old)->pfn_end), 1) : \
		(0);\
		\
		(range_start_pfn) = (range_end_pfn) + 1, \
		(void)(((rme_new) = advance_rme(rme_new, range_start_pfn)) && \
		((rme_old) = advance_rme(rme_old, range_start_pfn))))

#define ml_pair_for_each_delta(ml_new, ml_old, rme_new, rme_old, \
		range_start_pfn, range_end_pfn) \
	ml_pair_for_each(ml_new, ml_old, rme_new, rme_old, range_start_pfn, range_end_pfn)\
		if ((rme_new)->nid == (rme_old)->nid) { \
			/* do nothing, avoid trailing else */ \
		} else

int dnuma_online_required_nodes_and_zones(struct memlayout *old_ml,
		struct memlayout *new_ml)
{
	struct rangemap_entry *old, *new;
	unsigned long start_pfn, end_pfn;
	int r;

	ml_pair_for_each_delta(new_ml, old_ml, new, old, start_pfn, end_pfn) {
		r = dnuma_online_page_range(start_pfn, end_pfn, new);
		if (r)
			return r;
	}

#ifdef CONFIG_DNUMA_MANAGED_PAGE_UPDATE_DELAY
	{
		struct zone *zone;
		/*
		 * this calculation cannot be folded into the above because in addition
		 * to updating zones that will have pages moved _to_ them, we also need
		 * to update zones that have pages moved _from_ them.
		 */
		for_each_zone(zone)
			zone_update_managed_pages(zone, new_ml);

		update_per_zone_wmark_min();
	}
#endif

	return 0;
}

/*
 * Does not assume it is called with any locking (but can be called with zone
 * locks held, if needed)
 */
void dnuma_add_page_to_new_zone(struct page *page, int order,
				  struct zone *dest_zone,
				  int dest_nid)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned long pfn_base = pfn;

	grow_pgdat_and_zone(dest_zone, pfn, pfn + (1UL << order));

	for (; pfn < pfn_base + (1UL << order); pfn++)
		set_page_node(pfn_to_page(pfn), dest_nid);
}

/*
 * must be called with zone->lock held (and local irq disabled) and
 * memlayout's update_lock held
 */
static void remove_free_page_from_zone(struct memlayout *ml, struct zone *zone,
				       struct page *page, int order)
{
	/* XXX: watermarks? */

	list_del(&page->lru);
	zone->free_area[order].nr_free--;
	__ClearPageBuddy(page);

	__mod_zone_freepage_state(zone, -(1 << order),
			get_pageblock_migratetype(page));

	lookup_node_clear_order(page, order);
	ml_stat_add(MLSTAT_TRANSPLANT_FROM_FREELIST_REMOVE, ml, zone->node, order);
}

/*
 * __ref is to allow (__meminit) zone_pcp_update(), which we will have because
 * DYNAMIC_NUMA depends on MEMORY_HOTPLUG (and MEMORY_HOTPLUG makes __meminit a
 * nop).
 */
static void __ref add_free_page_to_node(struct memlayout *ml,
					int dest_nid, struct page *page,
					int order)
{
	struct zone *dest_zone = nid_zone(dest_nid, page_zonenum(page));

	VM_BUG_ON(!zone_is_initialized(dest_zone));

	dnuma_add_page_to_new_zone(page, order, dest_zone, dest_nid);
	return_pages_to_zone(page, order, dest_zone);
	ml_stat_add(MLSTAT_TRANSPLANT_FROM_FREELIST_ADD, ml, dest_nid, order);
#ifdef CONFIG_DNUMA_MANAGED_PAGE_UPDATE_DELAY
	zone_adjust_managed_page_count(dest_zone, 1 << order);

	/*
	 * TODO: only run after enough adjust_managed_page_count()s have
	 * occured, batch
	 */
	update_per_zone_wmark_min();
#endif
}

#ifdef CONFIG_DNUMA_STRICT_BOUNDS
/*
 * We avoid doing any hard work to try to split the pages optimally
 * here because the page allocator splits them into 0-order pages
 * anyway.
 */
static void add_split_pages_to_zones(
		struct memlayout *ml,
		struct rangemap_entry *first_rme,
		unsigned long base_pfn, int order)
{
	struct rangemap_entry *rme = first_rme;
	unsigned long pfn;
	for (pfn = base_pfn; pfn < base_pfn + (1 << order); pfn++) {
		struct page *page = pfn_to_page(pfn);
		int nid;
		while (rme && pfn > rme->pfn_end)
			rme = rme_next(rme);

		if (rme && pfn >= rme->pfn_start)
			nid = rme->nid;
		else {
			WARN(1, "last rme: "RME_FMT"; pfn: %05lx\n",
					RME_EXP(rme), pfn);
			nid = page_to_nid(page);
		}

		add_free_page_to_node(ml, nid, page, 0);
		ml_stat_add(MLSTAT_SPLIT_PAGES, ml, nid, 0);
	}
}
#endif

/*
 * provided we have the destination zone (according to the current memlayout)
 * locked, this will return the page order if it is in the freelist, otherwise
 * it will return 0.
 */
static int page_on_freelist(struct page *page)
{

}

static bool page_is_managed(struct page *page)
{
	/*
	 * FIXME: this is wrong, consider CMA pages (which are decremented from
	 * the managed_page count)
	 */
	return !PageReserved(page);
}

/*
 * Callers must hold lock_memory_hotplug() for stability of present_pages.
 *
 * Note that while we iterate over all pages and could collect the info to
 * shrink the spanned pfns (via spanned_pages and start_pfn fields),
 * because movement of pages from their old node to the new one occurs
 * gradually doing so would cause some allocated pages that still belong to a
 * node/zone being missed durring a iteration over the span.
 *
 * Alternately, simply enlarging the spanned pfns immediately is possible, but
 * would make iterations over nodes or zones via spanned pfns (immediately)
 * slower.
 *
 * We avoid processing each zone/node seperately (as the functions in
 * memory_hotplug do) because it is possible that many nodes and zones have
 * changed, and pessimistic node layouts could cause us to iterate over
 * (nearly) all pfns multiple times.
 *
 * managed_pages are not updated here because they are used to calculate
 * watermarks and reserves. We adjust managed_pages (and recalculate watermarks
 * and reserves) as we transplant pages.
 */
#define page_count_idx(nid, zone_num) (zone_num + MAX_NR_ZONES * (nid))
static void update_present_page_counts(struct memlayout *new_ml)
{
	/*
	 * Perform a combined iteration of pgdat+zones and memlayout.
	 * - memlayouts are ordered, their lookup from pfn is "slow", and they
	 *   are contiguous.
	 * - pgdat+zones are unordered, have O(1) lookups, and don't have holes
	 *   over valid pfns.
	 */
	int nid;
	bool need_zonelists_rebuild = false;
	struct rangemap_entry *rme;
	unsigned long pfn = 0;
	struct {
		unsigned long present_pages;
#ifndef CONFIG_DNUMA_MANAGED_PAGE_UPDATE_DELAY
		unsigned long managed_pages;
#endif
	} *ct = kzalloc(nr_node_ids * MAX_NR_ZONES * sizeof(*ct),
		       GFP_KERNEL);
	if (WARN_ON(!ct))
		return;
	rme = rme_first(new_ml);

	/*
	 * TODO: use knowledge about what size blocks of pages can be !valid to
	 * improve computation speed.
	 */
	for (pfn = 0; pfn < max_pfn; pfn++) {
		int nid;
		struct page *page;
		size_t idx;

		if (!pfn_valid(pfn))
			continue;

		page = pfn_to_page(pfn);
		if (pfn > rme->pfn_end)
			rme = rme_next(rme);

		if (WARN_ON(!rme))
			continue;

		nid = rme->nid;

		idx = page_count_idx(nid, page_zonenum(page));
#ifndef CONFIG_DNUMA_MANAGED_PAGE_UPDATE_DELAY
		if (page_is_managed(page))
			ct[idx].managed_pages++;
#endif
		ct[idx].present_pages++;
	}

	for (nid = 0; nid < nr_node_ids; nid++) {
		unsigned long flags;
		unsigned long nid_present = 0;
		int zone_num;
		pg_data_t *node = NODE_DATA(nid);
		if (!node)
			continue;
		for (zone_num = 0; zone_num < node->nr_zones;
				zone_num++) {
			struct zone *zone = &node->node_zones[zone_num];
			size_t idx = page_count_idx(nid, zone_num);
			bool need_init_pageset = !populated_zone(zone);
			if (need_init_pageset)
				need_zonelists_rebuild = true;

			pr_debug("nid %d zone %d mp=%lu pp=%lu -> pp=%lu mp=%ld\n",
					nid, zone_num,
					zone->managed_pages,
					zone->present_pages,
					ct[idx].present_pages,
#ifdef CONFIG_DNUMA_MANAGED_PAGE_UPDATE_DELAY
					-1
#else
					ct[idx].managed_pages
#endif
					);
#ifndef CONFIG_DNUMA_MANAGED_PAGE_UPDATE_DELAY
			/* FIXME: locking */
			zone->managed_pages = ct[idx].managed_pages;
#endif
			zone->present_pages = ct[idx].present_pages;
			nid_present += zone->present_pages;
			if (need_init_pageset
					&& zone_pageset_can_be_setup(zone)) {
				setup_zone_pageset(zone);
				ml_stat_inc(MLSTAT_PCP_SETUP, new_ml, nid);
			} else {
				/*
				 * recalculate pcp ->batch & ->high using
				 * zone->managed_pages
				 */
				zone_pcp_update(zone);
				ml_stat_inc(MLSTAT_PCP_UPDATE, new_ml, nid);
			}
		}

		/* TODO: there are other node_states that need setting/clearing */
		if (!node_state(nid, N_MEMORY))
			node_set_state(nid, N_MEMORY);

		pr_debug(" node %d zone * present_pages %lu to %lu\n",
				nid, node->node_present_pages,
				nid_present);
		pgdat_resize_lock(node, &flags);
		node->node_present_pages = nid_present;
		pgdat_resize_unlock(node, &flags);
	}

	if (need_zonelists_rebuild) {
		ml_stat_inc(MLSTAT_ZONELIST_REBUILD, new_ml, NUMA_NO_NODE);

		mutex_lock(&zonelists_mutex);
		build_all_zonelists(NULL, NULL);
		mutex_unlock(&zonelists_mutex);
	} else
		ml_stat_inc(MLSTAT_NO_ZONELIST_REBUILD, new_ml, NUMA_NO_NODE);

	kfree(ct);
}

static void lock_2_zones(struct zone *z1, struct zone *z2,
		unsigned long *flags)
{
	BUG_ON(z1 == z2);
	if (z1 > z2) {
		spin_lock_irqsave(&z1->lock, *flags);
		spin_lock_nested(&z2->lock, SINGLE_DEPTH_NESTING);
	} else {
		spin_lock_irqsave(&z2->lock, *flags);
		spin_lock_nested(&z1->lock, SINGLE_DEPTH_NESTING);
	}
}

/*
 * Returns the last pfn that was processed.
 *
 * Iterating over pfns in 3 range overlays
 * - new memory layout
 * - old memory layout
 * - higher order pages
 *
 * TODO: ensure this plays nice with migrate types.
 */
static int dnuma_transplant_pfn_range(struct memlayout *ml,
		nodemask_t *n, unsigned long *zones_emptied,
		unsigned long pfn_start, unsigned long pfn_end,
		struct rangemap_entry *old,
		struct rangemap_entry *new)
{
	unsigned long pfn = pfn_start;
	pr_devel("transplating pfn {%05lx - %05lx} from %d to %d\n", pfn_start,
			pfn_end, old->nid, new->nid);

	for (; pfn <= pfn_end; pfn++) {
		struct zone *old_zone, *new_zone;
		struct page *page;
		int page_nid, order;
		unsigned long flags, last_pfn_in_page, first_pfn_in_page;
		enum zone_type zone_num;

		ml_stat_inc(MLSTAT_TRANSPLANT_EXAMINED_PFN, ml, new->nid);

		if (!pfn_valid(pfn))
			continue;

		lookup_node_mark_pfn(pfn);
		page = pfn_to_page(pfn);

		/*
		 * TODO: examine changing the page_zone() directly for reserved
		 * pages They are marked, so if they ever get into the page
		 * allocator, their zone will be corrected.
		 */
		if (PageReserved(page)) {
			ml_stat_inc(MLSTAT_TRANSPLANT_BAIL_RESERVED, ml, new->nid);
			continue;
		}

		zone_num = page_zonenum(page);

		/*
		 * Only one transision of the page nid is possible:
		 * previous nid (which may or may not be old->nid) ==> new->nid
		 * Once at new->nid, no further transisions can occur (until
		 * the memlayout_lock is unlocked).
		 *
		 * We don't need to check PageBuddy ? pfn += page_order : 1
		 * because we will simply skip future pages in the same
		 * higher-order-page via the !PageBuddy() check below, and via
		 * this check repeated (pages that form a higher order page
		 * will all have the same nid & zone).
		 */
		page_nid = page_to_nid(page);
		if (page_nid == new->nid) {
			ml_stat_inc(MLSTAT_TRANSPLANT_BAIL_NID_EQ, ml, new->nid);
			continue;
		}

		old_zone = nid_zone(page_nid,  zone_num);
		new_zone = nid_zone(new->nid, zone_num);

		lock_2_zones(old_zone, new_zone, &flags);

		/* isolated pages are also caught by this */
		if (!PageBuddy(page)) {
			ml_stat_inc(MLSTAT_TRANSPLANT_BAIL_PAGE_NOT_BUDDY, ml, new->nid);
			goto skip_unlock;
		}

		/*
		 * It has already been transplanted "somewhere", somewhere
		 * should be the zone in the nid indicated by the new_ml
		 * (because we know a grace period has passed following our
		 * assingnment of the new memlayout).
		 */
		if (page_zone(page) != old_zone) {
			WARN_ON(page_zone(page) != new_zone);
			ml_stat_inc(MLSTAT_TRANSPLANT_BAIL_ALREADY_DONE, ml, new->nid);
			goto skip_unlock;
		}

		/* Locking new_zone was just to ensure we could check
		 * PageBuddy()+page_zone() atomically, we relock new_zone later
		 * for the actual free */
		spin_unlock(&new_zone->lock);

		order = page_order(page);
		first_pfn_in_page = pfn & ~((1 << order) - 1);
		last_pfn_in_page  = pfn |  ((1 << order) - 1);
		if (WARN(pfn != first_pfn_in_page,
					"pfn %05lx is not first_pfn %05lx\n",
					pfn, first_pfn_in_page)) {
			pfn = last_pfn_in_page;
			goto skip_unlock_old;
		}

		__node_set(new->nid, n);

		remove_free_page_from_zone(ml, old_zone, page, order);

		if (!populated_zone(old_zone) &&
#ifndef CONFG_DNUMA_MANAGED_PAGE_UPDATE_DELAY
				nr_free_pages_in_zone(old_zone) == 0) {
#else
				!WARN_ON(nr_free_pages_in_zone(old_zone)) {
#endif
			pr_info("drain zonestat of Node %d %s\n",
					old_zone->node, old_zone->name);
			ml_stat_inc(MLSTAT_DRAIN_ZONESTAT, ml, new->nid);
			__set_bit(zone_to_zone_pos(old_zone), zones_emptied);
		}

		spin_unlock_irqrestore(&old_zone->lock, flags);

#ifdef CONFIG_DNUMA_MANAGED_PAGE_UPDATE_DELAY
		zone_adjust_managed_page_count(old_zone, -(1 << order));
#endif

		if (last_pfn_in_page > pfn_end) {
			/*
			 * this higher order page doesn't fit into the current
			 * range even though it starts there.
			 */
			pr_warn("order-%02d page (pfn %05lx-%05lx) extends beyond end of delta {%05lx-%05lx} between rme "RME_FMT" and "RME_FMT"\n",
					order, first_pfn_in_page,
					last_pfn_in_page,
					pfn_start, pfn_end,
					RME_EXP(old), RME_EXP(new));
#ifdef CONFIG_DNUMA_STRICT_BOUNDS

			/*
			 * painfully, a higher order page can extend past the
			 * end of the region we are supposed to be examining,
			 * and potentially results in us iterating over the new
			 * rme's twice (once in add_split_pages_to_zones() and
			 * once in the function that calls this one)
			 */
			add_split_pages_to_zones(ml, new, pfn, order);
			return last_pfn_in_page;
#endif
		}

		add_free_page_to_node(ml, new->nid, page, order);

		pfn = last_pfn_in_page;
		continue;
skip_unlock:
		spin_unlock(&new_zone->lock);
skip_unlock_old:
		spin_unlock_irqrestore(&old_zone->lock, flags);
	}

	return pfn - 1;
}

/*
 * move pages that are free to the node indicated by new_ml.
 *
 * we only examine pages whose node has changed since the previous memlayout,
 * as the pages which differ from earlier memlayouts will
 *  1) have been moved by earlier invocations of this function OR
 *  2) have been moved on free by examining and clearing the lookup mark OR
 *  3) still have their lookup mark set and still be allocated
 */
int __ref dnuma_move_free_pages(struct memlayout *old_ml,
				struct memlayout *new_ml)
{
	struct rangemap_entry *old, *new;
	unsigned long start_pfn, end_pfn;
	nodemask_t nodes_added_to = NODE_MASK_NONE;
	int node;
	unsigned long *zones_emptied = kzalloc(
			BITS_TO_LONGS(MAX_ZONES) * sizeof(*zones_emptied),
			GFP_KERNEL);
	if (!zones_emptied)
		return -ENOMEM;

	update_present_page_counts(new_ml);
	reduce_per_zone_wmarks_and_reserves();

	ml_pair_for_each_delta(old_ml, new_ml, old, new, start_pfn, end_pfn)
		end_pfn = dnuma_transplant_pfn_range(new_ml, &nodes_added_to,
				zones_emptied, start_pfn, end_pfn, old, new);

	/*
	 * Sets 'min_free_kbytes' based on 'nr_free_buffer_pages()'
	 * - call setup_per_zone_wmarks()
	 *   - [also called when min_free_kbytes is changed via sysctl]
	 *   - set zone->watermark[WMARK_MIN] based on zone->managed_pages and
	 *     (sometimes) 'min_free_kbytes'.
	 *   - set zone->watermark[WMARK_{LOW,HIGH}] based on
	 *     min_wmark_pages(zone)
	 * - call refresh_zone_stat_thresholds()
	 *   - ???
	 * - call setup_per_zone_lowmem_reserve()
	 *   - set zone->lowmem_reserve[*] based on zone->managed_pages,
	 *     sysctl_lowmem_reserve_ratio, and other zone's managed_pages.
	 *   - call calculate_totalreserve_pages()
	 *     - set dirty_balace_reserve and totalreserve_pages based on
	 *       zone->managed_pages, high_wmark_pages(zone),
	 *       zone->lowmem_reserve (for every zone in the system).
	 * - call setup_per_zone_inactive_ratio()
	 *   - set zone->inactive_ratio based on zone->managed_pages
	 */
	update_per_zone_wmark_min(true);

	/* XXX: do we need this given that the total number of pages on the
	 * system didn't change? */
	/* XXX: this is taken from memory hotplug, what does it actually do? */
	for_each_node_mask(node, nodes_added_to)
		kswapd_run(node);

	/* XXX: this is taken from memory hotplug, what does it actually do? */
	writeback_set_ratelimit();

	zone_pcp_destroy_from_mask(zones_emptied);

	kfree(zones_emptied);
	return 0;
}
