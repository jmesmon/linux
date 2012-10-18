#define pr_fmt(fmt) "memlayout: " fmt

#include <linux/dnuma.h>
#include <linux/init.h>   /* __init */
#include <linux/kernel.h> /* sprintf */
#include <linux/memblock.h>
#include <linux/module.h> /* THIS_MODULE, needed for DEFINE_SIMPLE_ATTR */
#include <linux/printk.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

/* Need to map an index (in this case, memory ranges/regions) to the range set
 * it belongs to.  Overlapping is not allowed, so iterval/sequence trees are
 * not needed
 */

/* Reasoning:
 * - using memblock to look up the region associated with a address is
 *   slow (and they don't have a public function that does that)
 * - mips, sh, x86, ia64, s390, and score all discard memblock after
 *   __init time.
 */

/* XXX: Issues
 * - will kmalloc() and friends be avaliable?
 * - should we use some of our vm space? (or make that an option)?
 * - node locality concerns: per node allocation? <----
 * - how large is this, really?
 * - use a kmem_cache? or a custom allocator to split pages?
 */

/* protected by update_lock */
__rcu struct memlayout *pfn_to_node_map;

static DEFINE_MUTEX(update_lock);
#define ml_update_lock()   mutex_lock(&update_lock)
#define ml_update_unlock() mutex_unlock(&update_lock)
#define ml_update_is_locked() mutex_is_locked(&update_lock)

#define kfree_complete_rbtree(root, type, field)	\
		rbtree_postorder_apply_safe(root, type, field, kfree)

#define rbtree_postorder_for_each_entry_safe(pos, n, root, field)		\
	for (pos = rb_entry(rb_first_postorder(root), typeof(*pos), field),	\
	      n = rb_entry(rb_next_postorder(&pos->field), typeof(*pos), field);	\
	     &pos->field;							\
	     pos = n,								\
	      n = rb_entry(rb_next_postorder(&pos->field), typeof(*pos), field))

#define rbtree_postorder_apply_safe(root, type, field, applyable) do {		\
		type *__kcr_entry, *__kcr_next;					\
		rbtree_postorder_for_each_entry_safe(__kcr_entry, __kcr_next, root, field) { \
			applyable(__kcr_entry);					\
		}								\
	} while (0)

static void free_rme_tree(struct rb_root *root)
{
	kfree_complete_rbtree(root, struct rangemap_entry, node);
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
	if (WARN_ON(nid > MAX_NUMNODES))
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

static inline bool rme_bounds_pfn(struct rangemap_entry *rme, unsigned long pfn)
{
	return rme->pfn_start <= pfn && pfn <= rme->pfn_end;
}

int memlayout_pfn_to_nid_no_pageflags(unsigned long pfn)
{
	struct rb_node *node;
	struct memlayout *ml;
	struct rangemap_entry *rme;
	rcu_read_lock();
	ml = rcu_dereference(pfn_to_node_map);
	if (!ml || (ml->type == ML_INITIAL))
		goto out;

	/* FIXME: hack that assumes reading & writing a pointer is atomic */
	rme = ACCESS_ONCE(ml->cache);
	if (rme && rme_bounds_pfn(rme, pfn)) {
		rcu_read_unlock();
		return rme->nid;
	}

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
			int nid = rme->nid;
			/* greater_than_start && less_than_end.
			 *  the case (!greater_than_start  && !less_than_end)
			 *  is impossible */
			/* FIXME: here the ACCESS_ONCE is mainly for anotation,
			 * there aren't repeated uses that would cause issues.
			 * */
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

	if (ml->type == ML_INITIAL) {
		if (WARN(dnuma_has_memlayout(), "memlayout marked first is not first, ignoring.\n")) {
			memlayout_destroy(ml);
			return;
		}

		ml_update_lock();
		rcu_assign_pointer(pfn_to_node_map, ml);
		ml_update_unlock();
		return;
	}

	/* locks & unlocks *_memory_hotplug() */
	dnuma_online_required_nodes(ml);

	ml_update_lock();
	old_ml = rcu_dereference_protected(pfn_to_node_map,
			ml_update_is_locked());

	dnuma_move_to_new_ml(ml);

	/* FIXME: in this interviening time between dnuma_move_to_new_ml() and
	 * assiging a new pfn_to_node_map, pages would be freed to the old ml
	 * */

	/* must occur after dnuma_move_to_new_ml() as dnuma_move_to_new_ml()
	 * sets up uninitialized zones which would otherwise be used
	 * uninitialized in the free path */
	rcu_assign_pointer(pfn_to_node_map, ml);

	dnuma_mark_page_range(ml);

	/* Must be done after the free_lists are emptied of pages which need
	 * transplanting, otherwise pages could be reallocated from the wrong
	 * nodes. */
	drain_all_pages();

	ml_update_unlock();

	synchronize_rcu();
	memlayout_destroy(old_ml);
}

int __init_memblock memlayout_init_from_memblock(void)
{
	int i, nid, errs = 0;
	unsigned long start, end;
	struct memlayout *ml = memlayout_create(ML_INITIAL);
	if (WARN_ON(!ml))
		return -ENOMEM;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, &nid) {
		int r = memlayout_new_range(ml, start, end - 1, nid);
		if (r) {
			pr_err("failed to add range [%lx, %lx] in node %d to mapping\n",
					start, end, nid);
			errs++;
		} else
			pr_devel("added range [%lx, %lx] in node %d\n",
					start, end, nid);
	}

	memlayout_commit(ml);
	return errs;
}
