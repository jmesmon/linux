#define pr_fmt(fmt) "memlayout: " fmt
#define DEBUG 1

#include <linux/debugfs.h>
#include <linux/dnuma.h>
#include <linux/init.h>   /* __init */
#include <linux/kernel.h> /* sprintf */
#include <linux/memblock.h>
#include <linux/module.h> /* THIS_MODULE, needed for DEFINE_SIMPLE_ATTR */
#include <linux/printk.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

/* Need to map an index (in this case, memory ranges/regions) to the range set it belongs to.
 * Overlapping is not allowed, so iterval/sequence trees are not needed
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
 * - node locality concerns: per node allocation?
 * - how large is this, really?
 * - use a kmem_cache? or a custom allocator to split pages?
 */

/* Datastructure notes:
 * - rbtree of {node, start, end}.
 * - assumes no 'ranges' overlap, which is true for memblock
 *   - not so true for pg_data_t on some archs.
 */
struct rangemap_entry {
	struct rb_node node;
	unsigned long pfn_start;
	/* @pfn_end: inclusive, stored this way (instead of a count) to make
	 *           the lookup faster */
	unsigned long pfn_end;
	int nid;
};

static struct rb_root pfn_to_node_map, new_pfn_to_node_map;
static DEFINE_SPINLOCK(update_lock);

#if defined(CONFIG_DEBUG_FS)

static int dfs_range_get(void *data, u64 *val)
{
	*val = (int)data;
	return 0;
}
static struct dentry *root_dentry;
DEFINE_SIMPLE_ATTRIBUTE(range_fops, dfs_range_get, NULL, "%llu");

#define for_each_memlayout_range(rme) \
	for (rme = rb_entry(rb_first(&pfn_to_node_map), typeof(*rme), node); \
	     &rme->node;						     \
	     rme = rb_entry(rb_next(&rme->node), typeof(*rme), node))

static void memlayout_debugfs_create(struct dentry *base)
{
	struct rangemap_entry *rme;
	char name[BITS_PER_LONG / 4 + 2];
	struct dentry *rd;
	for_each_memlayout_range(rme) {
		sprintf(name, "%lX-%lX", rme->pfn_start, rme->pfn_end);
		rd = debugfs_create_file(name, 0400, base, (void *)rme->nid, &range_fops);
		if (!rd) {
			pr_devel("failed to create memlayout debugfs: {%lX-%lX}:%d", rme->pfn_start, rme->pfn_end, rme->nid);
			return;
		}
	}
}

static int __init memlayout_debugfs_init(void)
{
	if (!debugfs_initialized()) {
		pr_devel("debugfs not registered or disabled.");
		return 0;
	}

	root_dentry = debugfs_create_dir("memlayout", NULL);
	if (!root_dentry) {
		pr_devel("failed to create dir");
		return 0;
	}

	rcu_read_lock();
	memlayout_debugfs_create(root_dentry);
	rcu_read_unlock();

	return 0;
}

static void __exit memlayout_debugfs_exit(void)
{
	debugfs_remove_recursive(root_dentry);
}

module_init(memlayout_debugfs_init);
module_exit(memlayout_debugfs_exit);
#else
#error "FAIL?"
#endif

static int find_insertion_point(unsigned long pfn_start, unsigned long pfn_end, int nid, struct rb_node ***o_new, struct rb_node **o_parent)
{
	struct rb_node **new = &new_pfn_to_node_map.rb_node, *parent = NULL;
	struct rangemap_entry *rme;
	pr_debug("adding early range: {%lX-%lX}:%d", pfn_start, pfn_end, nid);
	while(*new) {
		rme = rb_entry(*new, typeof(*rme), node);

		parent = *new;
		if (pfn_end < rme->pfn_start && pfn_start < rme->pfn_end)
			new = &((*new)->rb_left);
		else if (pfn_start > rme->pfn_end && pfn_end > rme->pfn_end)
			new = &((*new)->rb_right);
		else {
			/* an embedded region, need to use an interval or
			 * sequence tree. */
			pr_warn("tried to embed {%lX,%lX}:%d inside {%lX-%lX}:%d",
				 pfn_start, pfn_end, nid,
				 rme->pfn_start, rme->pfn_end, rme->nid);
			return 1;
		}
	}

	*o_new = new;
	*o_parent = parent;
	return 0;
}

static int early_new_range(struct rangemap_entry *rme)
{
	struct rb_node **new, *parent;
	if (find_insertion_point(rme->pfn_start, rme->pfn_end, rme->nid, &new, &parent))
		return 1;

	rb_link_node(&rme->node, parent, new);
	rb_insert_color(&rme->node, &pfn_to_node_map);
	return 0;
}

int memlayout_new_range(unsigned long pfn_start, unsigned long pfn_end, int nid)
{
	struct rb_node **new, *parent;
	struct rangemap_entry *rme;
	if (find_insertion_point(pfn_start, pfn_end, nid, &new, &parent))
		return 1;

	rme = kmalloc(sizeof(*rme), GFP_KERNEL);
	if (!rme)
		return -ENOMEM;

	rme->pfn_start = pfn_start;
	rme->pfn_end = pfn_end;
	rme->nid = nid;

	rb_link_node(&rme->node, parent, new);
	rb_insert_color(&rme->node, &pfn_to_node_map);

	return 0;
}

int memlayout_pfn_to_nid_no_pageflags(unsigned long pfn)
{
	struct rb_node *node;
	rcu_read_lock();
	node = rcu_dereference(pfn_to_node_map.rb_node);
	while(node) {
		struct rangemap_entry *rme = rb_entry(node, typeof(*rme), node);
		bool gts = rme->pfn_start <= pfn;
		bool lte = pfn <= rme->pfn_end;

		if (gts && !lte)
			node = node->rb_right;
		else if (!lte && gts)
			node = node->rb_left;
		else { /* gts && lte, the case (!gts && !lte) is impossible */
			rcu_read_unlock();
			return rme->nid;
		}
	}

	rcu_read_unlock();
	return NUMA_NO_NODE;
}

/* always visit the parent after it's children */
static struct rb_node *rb_next_postorder(struct rb_node *node)
{
	struct rb_node *new, *parent;
	if (!node)
		return NULL;
	parent = rb_parent(node);

	/* If we're sitting on node, we've already seen our children */
	if (parent && node == parent->rb_left) {
		/* If we are the parent's left node, go to the parent's right
		 * node then all the way to the left */
		new = parent->rb_right;
		while(new->rb_left)
			new = new->rb_left;
		return new;
	} else
		/* Otherwise we are the parent's right node, and the parent
		 * should be next */
		return parent;
}

static struct rb_node *rb_least(struct rb_node *node)
{
	struct rb_node *new = node;
	if (!node)
		return NULL;
	while (new->rb_left)
		new = new->rb_left;
	return new;
}

static void free_rme_tree(struct rb_node *root)
{
	struct rb_node *node, *next;
	for (node = rb_least(root), next = rb_next_postorder(node);
	     node;
	     node = next, next = rb_next_postorder(node)) {
		struct rangemap_entry *rme = rb_entry(node, typeof(*rme), node);
		kfree(rme);
	}
}

/* fiddling with rb_node inside the rb_root is done to avoid what is
 * (currently) an unneeded secondary dereference.  */
void memlayout_commit(void)
{
	struct rb_node *root;
	unsigned long flags;

	spin_lock_irqsave(&update_lock, flags);
	root = rcu_dereference_protected(pfn_to_node_map.rb_node,
			spin_is_locked(&update_lock));
	rcu_assign_pointer(pfn_to_node_map.rb_node,
			new_pfn_to_node_map.rb_node);
	spin_unlock_irqrestore(&update_lock, flags);

	synchronize_rcu();
	free_rme_tree(root);
}

int __init_memblock memlayout_init_from_memblock(void)
{
	int i, nid, errs = 0;
	unsigned long start, end;
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, &nid) {
		int r = memlayout_new_range(start, end - 1, nid);
		if (r) {
			pr_err("failed to add range [%lx, %lx] in node %d to mapping",
					start, end, nid);
			errs++;
		} else
			pr_devel("added ranged [%lx, %lx] in node %d",
					start, end, nid);
	}

	memlayout_commit();
	return errs;
}
