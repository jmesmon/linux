#ifndef LINUX_DNUMA_H_
#define LINUX_DNUMA_H_
#define DEBUG 1

#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/memlayout.h>
#include <linux/spinlock.h>

#ifdef CONFIG_DYNAMIC_NUMA
extern u64 dnuma_moved_page_ct;
extern spinlock_t dnuma_stats_lock;

/* called by memlayout_commit() _before_ a new memory layout takes effect. */
void dnuma_online_required_nodes(struct memlayout *new_ml);

/* called by memlayout_commit() _after_ a new memory layout takes effect. */
/* called with lock_memory_hotplug() & rcu_read_lock() both locked */
void dnuma_move_to_new_ml(struct memlayout *new_ml);

static inline bool dnuma_is_active(void)
{
	struct memlayout *ml;
	bool ret;

	rcu_read_lock();
	ml = rcu_dereference(pfn_to_node_map);
	ret = ml && (ml->type != ML_INITIAL);
	rcu_read_unlock();

	return ret;
}

static inline bool dnuma_has_memlayout(void)
{
	struct memlayout *ml;
	rcu_read_lock();
	ml = rcu_dereference(pfn_to_node_map);
	rcu_read_unlock();

	return !!ml;
}

static inline struct zone *get_zone(int nid, enum zone_type zonenum)
{
	return &NODE_DATA(nid)->node_zones[zonenum];
}

static inline int dnuma_page_needs_move(struct page *page)
{
	int new_nid, old_nid;

	if (!dnuma_is_active())
		return NUMA_NO_NODE;

	new_nid = memlayout_pfn_to_nid_no_pageflags(page_to_pfn(page));
	old_nid = page_to_nid(page);

	if (new_nid == old_nid)
		return NUMA_NO_NODE;

	pr_debug("checking new_nid %d and zonenum %d\n", new_nid, page_zonenum(page));
	/* While a current memlayout is changing, a zone might not yet be
	 * initialized, so we must avoid moving pages to it. */
	if (!zone_is_initialized(get_zone(new_nid, page_zonenum(page))))
		return NUMA_NO_NODE;

	return new_nid;
}

#if CONFIG_DNUMA_DEBUGFS
static inline void dnuma_update_move_page_stats(void)
{
	unsigned long flags;
	spin_lock_irqsave(&dnuma_stats_lock, flags);
	if (dnuma_moved_page_ct < (~(u64)0))
		dnuma_moved_page_ct++;
	spin_unlock_irqrestore(&dnuma_stats_lock, flags);
}
#else
static inline void dnuma_update_move_page_stats(void)
{}
#endif

/* here, "add" implies that the page was already free. */
static inline void dnuma_prior_add_to_new_zone(struct page *page, int order,
		struct zone *dest_zone, int dest_nid)
{
	int i;
	unsigned long pfn;
	for (i = 0; i < 1 << order; i++)
		set_page_node(page + i, dest_nid);

	pfn = page_to_pfn(page);
	grow_pgdat_and_zone(dest_zone, pfn, pfn + (1 << order));
	dnuma_update_move_page_stats();
}

static inline struct zone *dnuma_prior_free_to_new_zone(struct page *page,
							int dest_nid)
{
	struct zone *dest_zone = get_zone(dest_nid, page_zonenum(page));
	struct zone *curr_zone = page_zone(page);

	/* XXX: lock bouncing: this & grow_pgdat_and_zone() both fiddle with the span_lock */
	adjust_zone_present_pages(curr_zone, -1);
	dnuma_prior_add_to_new_zone(page, 0, dest_zone, dest_nid);
	return dest_zone;
}

static inline void dnuma_post_free_to_new_zone(struct zone *dest_zone)
{
	adjust_zone_present_pages(dest_zone, +1);
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
	if (new_nid != old_nid)
		dnuma_prior_free_to_new_zone(page, new_nid);

	return get_zone(new_nid, zonenum);
}
#else
static inline bool dnuma_is_active(void)
{
	return false;
}

static inline struct zone *dnuma_move_free_page_zone(struct page *page)
{
	return page_zone(page);
}

static inline struct zone *dnuma_prior_free_to_new_zone(struct page *page, int dest_nid)
{
	BUG();
	return NULL;
}

static inline bool dnuma_page_needs_move(struct page *page, int *dest_nid)
{
	return false;
}
#endif

#endif
