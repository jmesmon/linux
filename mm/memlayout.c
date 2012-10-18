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
	if (WARN_ON(nid >= nr_node_ids))
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
	return 0;
}

/*
 * If @ml is the pfn_to_node_map, it must have been dereferenced and
 * rcu_read_lock() must be held when called and while the returned
 * rangemap_entry is used. Alternately, the update_lock can be held and
 * rcu_dereference_protected() used for operations that need to block.
 *
 * Returns the RME that contains the given PFN,
 * OR if there is no RME that contains the given PFN, it returns the next one
 *    (containing a higher pfn),
 * OR if there is no next RME, it returns NULL.
 *
 * This is designed for use in iterating over a subset of the rme's, starting
 * at @pfn passed to this function.
 */
struct rangemap_entry *memlayout_pfn_to_rme_higher(struct memlayout *ml,
						   unsigned long pfn)
{
	struct rb_node *node, *prev_node = NULL;
	struct rangemap_entry *rme;
	if (!ml || (ml->type == ML_INITIAL))
		return NULL;

	rme = ACCESS_ONCE(ml->cache);
	smp_read_barrier_depends();

	if (rme && rme_bounds_pfn(rme, pfn))
		return rme;

	node = ml->root.rb_node;
	while (node) {
		struct rangemap_entry *rme = rb_entry(node, typeof(*rme),
						      node);
		bool greater_than_start = rme->pfn_start <= pfn;
		bool less_than_end = pfn <= rme->pfn_end;

		if (greater_than_start && !less_than_end) {
			prev_node = node;
			node = node->rb_right;
		} else if (less_than_end && !greater_than_start) {
			prev_node = node;
			node = node->rb_left;
		} else {
			/* only can occur if a range ends before it starts */
			if (WARN_ON(!greater_than_start && !less_than_end))
				return NULL;

			/* greater_than_start && less_than_end. */
			ACCESS_ONCE(ml->cache) = rme;
			return rme;
		}
	}
	if (prev_node) {
		struct rangemap_entry *rme = rb_entry(prev_node, typeof(*rme),
						      node);
		if (pfn < rme->pfn_start)
			return rme;
		else
			return rme_next(rme);
	}
	return NULL;
}

static struct rangemap_entry *memlayout_pfn_to_rme(struct memlayout *ml,
		unsigned long pfn)
{
	struct rangemap_entry *rme = memlayout_pfn_to_rme_higher(ml, pfn);
	/*
	 * by using a modified version of memlayout_pfn_to_rme_higher(), the
	 * rme_bounds_pfn() check could be skipped. Unfortunately, it would also
	 * result in a large amount of copy-pasted code (or a nasty inline func)
	 */
	if (!rme || !rme_bounds_pfn(rme, pfn))
		return NULL;
	return rme;
}

int memlayout_pfn_to_nid_if_active(unsigned long pfn)
{
	struct memlayout *ml;
	struct rangemap_entry *rme;
	int nid = NUMA_NO_NODE;

	rcu_read_lock();
	ml = memlayout_rcu_deref_if_active();
	if (!ml)
		goto out;

	rme = memlayout_pfn_to_rme(ml, pfn);
	if (!rme)
		goto out;

	nid = rme->nid;
out:
	rcu_read_unlock();
	return nid;
}

int memlayout_pfn_to_nid(unsigned long pfn)
{
	struct rangemap_entry *rme;
	int nid = NUMA_NO_NODE;

	rcu_read_lock();
	rme = memlayout_pfn_to_rme(rcu_dereference(pfn_to_node_map), pfn);
	if (!rme)
		goto out;

	nid = rme->nid;
out:
	rcu_read_unlock();
	return nid;
}

/*
 * given a new memory layout that is not yet in use by the system,
 * modify it so that
 * - all pfns are included
 *   - handled by extending the first range to the beginning of memory and
 *     extending all other ranges until they abut the following range (or in the
 *     case of the last node, the end of memory)
 *
 * 1) we could have it exclude pfn ranges that are !pfn_valid() if we hook
 * into the code which changes pfn validity.
 *  - Would this be a significant performance/code quality boost?
 *
 * 2) even further, we could munge the memlayout to handle cases where the
 * number of physical numa nodes exceeds nr_node_ids, and generally clean up
 * the node numbering (avoid nid gaps, renumber nids to reduce the need for
 * moving pages between nodes). These changes would require cooperation between
 * this and code which manages the mapping of CPUs to nodes.
 */
static void memlayout_expand(struct memlayout *ml)
{
	struct rb_node *r = rb_first(&ml->root);
	struct rangemap_entry *rme = rb_entry(r, typeof(*rme), node), *prev;
	if (rme->pfn_start != 0) {
		pr_info("expanding rme "RME_FMT" to start of memory\n",
				RME_EXP(rme));
		rme->pfn_start = 0;
	}

	for (r = rb_next(r); r; r = rb_next(r)) {
		prev = rme;
		rme = rb_entry(r, typeof(*rme), node);

		if (prev->pfn_end + 1 < rme->pfn_start) {
			pr_info("expanding rme "RME_FMT" to end of "RME_FMT"\n",
					RME_EXP(prev), RME_EXP(rme));
			prev->pfn_end = rme->pfn_start - 1;
		}
	}

	if (rme->pfn_end < max_pfn) {
		pr_info("expanding rme "RME_FMT" to max_pfn=%05lx\n",
				RME_EXP(rme), max_pfn);
		rme->pfn_end = max_pfn;
	}
}

void memlayout_destroy(struct memlayout *ml)
{
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

	return ml;
}

void memlayout_commit(struct memlayout *ml)
{
	struct memlayout *old_ml;
	memlayout_expand(ml);

	if (ml->type == ML_INITIAL) {
		if (WARN(memlayout_exists(),
				"memlayout marked first is not first, ignoring.\n")) {
			memlayout_destroy(ml);
			return;
		}

		mutex_lock(&memlayout_lock);
		rcu_assign_pointer(pfn_to_node_map, ml);
		mutex_unlock(&memlayout_lock);
		return;
	}

	lock_memory_hotplug();
	mutex_lock(&memlayout_lock);

	old_ml = rcu_dereference_protected(pfn_to_node_map,
			mutex_is_locked(&memlayout_lock));

	dnuma_online_required_nodes_and_zones(old_ml, ml);

	rcu_assign_pointer(pfn_to_node_map, ml);

	synchronize_rcu();

	dnuma_move_free_pages(old_ml, ml);

	/* All new _non pcp_ page allocations now match the memlayout*/
	drain_all_pages();
	/* All new page allocations now match the memlayout */

	memlayout_destroy(old_ml);

	mutex_unlock(&memlayout_lock);
	unlock_memory_hotplug();
}

/*
 * The default memlayout global initializer, using memblock to determine
 * affinities
 *
 * reqires: slab_is_available() && memblock is not (yet) freed.
 * sleeps: definitely: memlayout_commit() -> synchronize_rcu()
 *	   potentially: kmalloc()
 */
__weak __init
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
