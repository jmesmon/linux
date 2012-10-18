#ifndef LINUX_DNUMA_H_
#define LINUX_DNUMA_H_

#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/memlayout.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

#ifdef CONFIG_DYNAMIC_NUMA
/* Must be called _before_ setting a new_ml to the pfn_to_node_map */
void dnuma_online_required_nodes_and_zones(struct memlayout *new_ml);

/* Must be called _after_ setting a new_ml to the pfn_to_node_map */
void dnuma_move_free_pages(struct memlayout *new_ml);
void dnuma_mark_page_range(struct memlayout *new_ml);

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
	return !!rcu_access_pointer(pfn_to_node_map);
}

static inline int dnuma_page_needs_move(struct page *page)
{
	int new_nid, old_nid;

	if (!TestClearPageLookupNode(page))
		return NUMA_NO_NODE;

	/* FIXME: this does rcu_lock, deref, unlock */
	if (WARN_ON(!dnuma_is_active()))
		return NUMA_NO_NODE;

	/* FIXME: and so does this (rcu lock, deref, and unlock) */
	new_nid = memlayout_pfn_to_nid(page_to_pfn(page));
	old_nid = page_to_nid(page);

	if (new_nid == NUMA_NO_NODE) {
		pr_alert("dnuma: pfn %05lx has moved from node %d to a non-memlayout range.\n",
				page_to_pfn(page), old_nid);
		return NUMA_NO_NODE;
	}

	if (new_nid == old_nid)
		return NUMA_NO_NODE;

	if (WARN_ON(!zone_is_initialized(nid_zone(new_nid, page_zonenum(page)))))
		return NUMA_NO_NODE;

	return new_nid;
}

void dnuma_post_free_to_new_zone(struct page *page, int order);
void dnuma_prior_free_to_new_zone(struct page *page, int order,
				  struct zone *dest_zone,
				  int dest_nid);

#else /* !defined CONFIG_DYNAMIC_NUMA */

static inline bool dnuma_is_active(void)
{
	return false;
}

static inline void dnuma_prior_free_to_new_zone(struct page *page, int order,
						struct zone *dest_zone,
						int dest_nid)
{
	BUG();
}

static inline void dnuma_post_free_to_new_zone(struct page *page, int order)
{
	BUG();
}

static inline int dnuma_page_needs_move(struct page *page)
{
	return NUMA_NO_NODE;
}
#endif /* !defined CONFIG_DYNAMIC_NUMA */

#endif /* defined LINUX_DNUMA_H_ */
