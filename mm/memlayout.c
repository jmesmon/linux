/*
 * memlayout - provides a mapping of PFN ranges to nodes with the requirements
 * that looking up a node from a PFN is fast, and changes to the mapping will
 * occour relatively infrequently.
 *
 */
#define pr_fmt(fmt) "memlayout: " fmt

#include <linux/dnuma.h>
#include <linux/export.h>
#include <linux/memblock.h>
#include <linux/printk.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

#include "memlayout-debugfs.h"

/* protected by memlayout_lock */
__rcu struct memlayout *pfn_to_node_map;
DEFINE_MUTEX(memlayout_lock);

static void free_rme_tree(struct rb_root *root)
{
	struct rangemap_entry *pos, *n;
	rbtree_postorder_for_each_entry_safe(pos, n, root, node) {
		kfree(pos);
	}
}

static void ml_destroy_mem(struct memlayout *ml)
{
	if (!ml)
		return;
	free_rme_tree(&ml->root);
	kfree(ml);
}

static int find_insertion_point(struct memlayout *ml, unsigned long pfn_start,
		unsigned long pfn_end, int nid, struct rb_node ***o_new,
		struct rb_node **o_parent)
{
	struct rb_node **new = &ml->root.rb_node, *parent = NULL;
	struct rangemap_entry *rme;
	pr_debug("adding range: {%lX-%lX}:%d\n", pfn_start, pfn_end, nid);
	while (*new) {
		rme = rb_entry(*new, typeof(*rme), node);

		parent = *new;
		if (pfn_end < rme->pfn_start && pfn_start < rme->pfn_end)
			new = &((*new)->rb_left);
		else if (pfn_start > rme->pfn_end && pfn_end > rme->pfn_end)
			new = &((*new)->rb_right);
		else {
			/* an embedded region, need to use an interval or
			 * sequence tree. */
			pr_warn("tried to embed {%lX,%lX}:%d inside {%lX-%lX}:%d\n",
				 pfn_start, pfn_end, nid,
				 rme->pfn_start, rme->pfn_end, rme->nid);
			return 1;
		}
	}

	*o_new = new;
	*o_parent = parent;
	return 0;
}

int memlayout_new_range(struct memlayout *ml, unsigned long pfn_start,
		unsigned long pfn_end, int nid)
{
	struct rb_node **new, *parent;
	struct rangemap_entry *rme;

	if (WARN_ON(nid < 0))
		return -EINVAL;
	if (WARN_ON(nid >= MAX_NUMNODES))
		return -EINVAL;

	if (find_insertion_point(ml, pfn_start, pfn_end, nid, &new, &parent))
		return 1;

	rme = kmalloc(sizeof(*rme), GFP_KERNEL);
	if (!rme)
		return -ENOMEM;

	rme->pfn_start = pfn_start;
	rme->pfn_end = pfn_end;
	rme->nid = nid;

	rb_link_node(&rme->node, parent, new);
	rb_insert_color(&rme->node, &ml->root);

	ml_dbgfs_create_range(ml, rme);
	return 0;
}

static inline bool rme_bounds_pfn(struct rangemap_entry *rme, unsigned long pfn)
{
	return rme->pfn_start <= pfn && pfn <= rme->pfn_end;
}

int memlayout_pfn_to_nid(unsigned long pfn)
{
	struct rb_node *node;
	struct memlayout *ml;
	struct rangemap_entry *rme;
	rcu_read_lock();
	ml = rcu_dereference(pfn_to_node_map);
	if (!ml || (ml->type == ML_INITIAL))
		goto out;

	rme = ACCESS_ONCE(ml->cache);
	if (rme && rme_bounds_pfn(rme, pfn)) {
		rcu_read_unlock();
		ml_stat_cache_hit();
		return rme->nid;
	}

	ml_stat_cache_miss();

	node = ml->root.rb_node;
	while (node) {
		struct rangemap_entry *rme = rb_entry(node, typeof(*rme), node);
		bool greater_than_start = rme->pfn_start <= pfn;
		bool less_than_end = pfn <= rme->pfn_end;

		if (greater_than_start && !less_than_end)
			node = node->rb_right;
		else if (less_than_end && !greater_than_start)
			node = node->rb_left;
		else {
			/* greater_than_start && less_than_end.
			 *  the case (!greater_than_start  && !less_than_end)
			 *  is impossible */
			int nid = rme->nid;
			ACCESS_ONCE(ml->cache) = rme;
			rcu_read_unlock();
			return nid;
		}
	}

out:
	rcu_read_unlock();
	return NUMA_NO_NODE;
}

void memlayout_destroy(struct memlayout *ml)
{
	ml_destroy_dbgfs(ml);
	ml_destroy_mem(ml);
}

struct memlayout *memlayout_create(enum memlayout_type type)
{
	struct memlayout *ml;

	if (WARN_ON(type < 0 || type >= ML_NUM_TYPES))
		return NULL;

	ml = kmalloc(sizeof(*ml), GFP_KERNEL);
	if (!ml)
		return NULL;

	ml->root = RB_ROOT;
	ml->type = type;
	ml->cache = NULL;

	ml_dbgfs_init(ml);
	return ml;
}

void memlayout_commit(struct memlayout *ml)
{
	struct memlayout *old_ml;

	if (ml->type == ML_INITIAL) {
		if (WARN(dnuma_has_memlayout(), "memlayout marked first is not first, ignoring.\n")) {
			ml_backlog_feed(ml);
			return;
		}

		mutex_lock(&memlayout_lock);
		ml_dbgfs_set_current(ml);
		rcu_assign_pointer(pfn_to_node_map, ml);
		mutex_unlock(&memlayout_lock);
		return;
	}

	lock_memory_hotplug();
	dnuma_online_required_nodes_and_zones(ml);
	unlock_memory_hotplug();

	mutex_lock(&memlayout_lock);

	ml_dbgfs_set_current(ml);

	old_ml = rcu_dereference_protected(pfn_to_node_map,
			mutex_is_locked(&memlayout_lock));

	rcu_assign_pointer(pfn_to_node_map, ml);

	synchronize_rcu();
	ml_backlog_feed(old_ml);

	/* Must be called only after the new value for pfn_to_node_map has
	 * propogated to all tasks, otherwise some pages may lookup the old
	 * pfn_to_node_map on free & not transplant themselves to their new-new
	 * node. */
	dnuma_mark_page_range(ml);

	/* Do this after the free path is set up so that pages are free'd into
	 * their "new" zones so that after this completes, no free pages in the
	 * wrong zone remain. */
	dnuma_move_free_pages(ml);

	/* All new _non pcp_ page allocations now match the memlayout*/
	drain_all_pages();
	/* All new page allocations now match the memlayout */

	mutex_unlock(&memlayout_lock);
}

/*
 * The default memlayout global initializer, using memblock to determine affinities
 * reqires: slab_is_available() && memblock is not (yet) freed.
 * sleeps: definitely: memlayout_commit() -> synchronize_rcu()
 *	   potentially: kmalloc()
 */
__weak __meminit
void memlayout_global_init(void)
{
	int i, nid, errs = 0;
	unsigned long start, end;
	struct memlayout *ml = memlayout_create(ML_INITIAL);
	if (WARN_ON(!ml))
		return;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, &nid) {
		int r = memlayout_new_range(ml, start, end - 1, nid);
		if (r) {
			pr_err("failed to add range [%05lx, %05lx] in node %d to mapping\n",
					start, end, nid);
			errs++;
		} else
			pr_devel("added range [%05lx, %05lx] in node %d\n",
					start, end, nid);
	}

	memlayout_commit(ml);
}

#ifdef CONFIG_MEMORY_HOTPLUG
/*
 * Provides a default memory_add_physaddr_to_nid() for memory hotplug, unless
 * overridden by the arch.
 */
__weak
int memory_add_physaddr_to_nid(u64 start)
{
	int nid = memlayout_pfn_to_nid(PFN_DOWN(start));
	if (nid == NUMA_NO_NODE)
		return 0;
	return nid;
}
EXPORT_SYMBOL_GPL(memory_add_physaddr_to_nid);
#endif
