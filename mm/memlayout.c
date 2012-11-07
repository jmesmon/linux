#define pr_fmt(fmt) "memlayout: " fmt
#define DEBUG 1

#include <linux/atomic.h>
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

#ifdef CONFIG_DEBUG_FS
#define HAVE_DEBUG_FS 1
#else
#define HAVE_DEBUG_FS 0
#endif

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
 * - node locality concerns: per node allocation? <----
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

/* protected by update_lock */
static __rcu struct memlayout *pfn_to_node_map;

#ifdef CONFIG_DEBUG_FS
static DEFINE_MUTEX(update_lock);
# define ml_update_lock()   mutex_lock(&update_lock)
# define ml_update_unlock() mutex_unlock(&update_lock)
# define ml_update_is_locked() mutex_is_locked(&update_lock)
#else /* !defined(CONFIG_DEBUG_FS) */
static DEFINE_SPINLOCK(update_lock);
# define ml_update_lock()   spin_lock(&update_lock)
# define ml_update_unlock() spin_unlock(&update_lock)
# define ml_update_is_locked() spin_is_locked(&update_lock)
#endif

#ifdef CONFIG_DEBUG_FS
static atomic_t ml_seq = ATOMIC_INIT(0);
static inline void ml_dbgfs_init(struct memlayout *ml)
{
	ml->seq = atomic_inc_return(&ml_seq) - 1;
	ml->d = NULL;
}

static int dfs_range_get(void *data, u64 *val)
{
	*val = (int)data;
	return 0;
}
static struct dentry *root_dentry;
DEFINE_SIMPLE_ATTRIBUTE(range_fops, dfs_range_get, NULL, "%Ld\n");

#define ml_for_each_range(ml, rme) \
	for (rme = rb_entry(rb_first(&ml->root), typeof(*rme), node); \
	     &rme->node;						     \
	     rme = rb_entry(rb_next(&rme->node), typeof(*rme), node))

static void _ml_dbgfs_create_range(struct dentry *base, struct rangemap_entry *rme, char *name)
{
	struct dentry *rd;
	sprintf(name, "%lX-%lX", rme->pfn_start, rme->pfn_end);
	rd = debugfs_create_file(name, 0400, base,
				(void*)rme->nid, &range_fops);
	if (!rd)
		pr_devel("debugfs: failed to create {%lX-%lX}:%d",
				rme->pfn_start, rme->pfn_end, rme->nid);
	else
		pr_devel("debugfs: created {%lX-%lX}:%d",
				rme->pfn_start, rme->pfn_end, rme->nid);
}

#define ML_LAYOUT_NAME_SZ ((size_t)(DIV_ROUND_UP(sizeof(unsigned) * 8, 3) + 1 + strlen("layout.")))
#define ML_REGION_NAME_SZ ((size_t)(2 * BITS_PER_LONG / 4 +2))
static void ml_layout_name(struct memlayout *ml, char *name)
{
	sprintf(name, "layout.%u", ml->seq);
}

static void ml_dbgfs_create_range(struct memlayout *ml, struct rangemap_entry *rme)
{
	char name[ML_REGION_NAME_SZ];
	_ml_dbgfs_create_range(ml->d, rme, name);
}

static void _ml_dbgfs_set_current(struct memlayout *ml, char *name)
{
	ml_layout_name(ml, name);
	/* XXX: remove old symlink? or will this overwrite? (overwriting would
	 * be nice) */
	debugfs_create_symlink("current", root_dentry, name);
}

static void ml_dbgfs_set_current(struct memlayout *ml)
{
	char name[ML_LAYOUT_NAME_SZ];
	_ml_dbgfs_set_current(ml, name);
}

/* create the entire current memlayout.
 * only used for the layout which exsists prior to fs initialization
 */
static void ml_dbgfs_create_initial(void)
{
	struct rangemap_entry *rme;
	struct dentry *base;
	char name[max(ML_REGION_NAME_SZ, ML_LAYOUT_NAME_SZ)];
	struct memlayout *old_ml, *new_ml;

	new_ml = kmalloc(sizeof(*new_ml), GFP_KERNEL);
	if (WARN(!new_ml, "memlayout allocation failed: "))
		return;

	ml_update_lock();

	old_ml = rcu_dereference_protected(pfn_to_node_map,
			ml_update_is_locked());
	if (WARN_ON(!old_ml))
		goto e_out;
	*new_ml = *old_ml;

	ml_layout_name(new_ml, name);
	base = debugfs_create_dir(name, root_dentry);
	if (!base)
		goto e_out;

	new_ml->d = base;

	ml_for_each_range(new_ml, rme) {
		_ml_dbgfs_create_range(base, rme, name);
	}

	_ml_dbgfs_set_current(new_ml, name);
	rcu_assign_pointer(pfn_to_node_map, new_ml);
	ml_update_unlock();

	synchronize_rcu();
	kfree(old_ml);

e_out:
	ml_update_unlock();
	kfree(new_ml);
}

#if defined(CONFIG_DNUMA_USER_WRITE)

#define DEFINE_DEBUGFS_GET(___type)					\
	static int debugfs_## ___type ## _get(void *data, u64 *val)	\
	{								\
		*val = *(___type *)data;				\
		return 0;						\
	}

DEFINE_DEBUGFS_GET(u32);
DEFINE_DEBUGFS_GET(u8);

#define DEFINE_WATCHED_ATTR(___type, ___var)			\
	static int ___var ## _watch_set(void *data, u64 val)	\
	{							\
		___type old_val = *(___type *)data;		\
		int ret = ___var ## _watch(old_val, val);	\
		if (!ret)					\
			*(___type *)data = val;			\
		return ret;					\
	}							\
	DEFINE_SIMPLE_ATTRIBUTE(___var ## _fops,		\
			debugfs_ ## ___type ## _get,		\
			___var ## _watch_set, "%llu\n");

static u64 dnuma_user_start;
static u64 dnuma_user_end;
static u32 dnuma_user_node; /* XXX: I don't care about this var, remove? */
static u8  dnuma_user_commit; /* XXX: don't care about this one either */
static struct memlayout *user_ml;
static int dnuma_user_node_watch(u32 old_val, u32 new_val)
{
	int ret;
	/* XXX: check if 'new_val' is an allocated node. */
	if (!user_ml)
		user_ml = ml_create();

	if (WARN_ON(!user_ml))
		return -ENOMEM;

	ret = memlayout_new_range(user_ml, dnuma_user_start, dnuma_user_end, new_val);
	if (ret)
		return ret;

	dnuma_user_start = 0;
	dnuma_user_end = 0;
	return 0;
}

static int dnuma_user_commit_watch(u8 old_val, u8 new_val)
{
	memlayout_commit(user_ml);
	user_ml = NULL;
	return 0;
}

DEFINE_WATCHED_ATTR(u32, dnuma_user_node);
DEFINE_WATCHED_ATTR(u8, dnuma_user_commit);
#endif

static int __init memlayout_debugfs_init(void)
{
	if (!debugfs_initialized()) {
		pr_devel("debugfs not registered or disabled.");
		return 0;
	}

	root_dentry = debugfs_create_dir("memlayout", NULL);
	if (!root_dentry)
		return 0;

	/* place in a different dir: try to keep memlayout & dnuma seperate */
	debugfs_create_u64("moved-pages", 0400, root_dentry, &dnuma_moved_page_ct);

#if defined(CONFIG_DNUMA_USER_WRITE)
	/* Set node last: on write, it adds the range. */
	debugfs_create_x64("start", 0600, root_dentry, &dnuma_user_start);
	debugfs_create_x64("end",   0600, root_dentry, &dnuma_user_end);
	debugfs_create_file("node",  0200, root_dentry,
			&dnuma_user_node, &dnuma_user_node_fops);
	debugfs_create_file("commit",  0200, root_dentry,
			&dnuma_user_commit, &dnuma_user_commit_fops);
#endif

	/* uses root_dentry */
	ml_dbgfs_create_initial();

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

static int find_insertion_point(struct memlayout *ml, unsigned long pfn_start, unsigned long pfn_end, int nid, struct rb_node ***o_new, struct rb_node **o_parent)
{
	struct rb_node **new = &ml->root.rb_node, *parent = NULL;
	struct rangemap_entry *rme;
	pr_debug("adding range: {%lX-%lX}:%d", pfn_start, pfn_end, nid);
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

#if 0
static int early_new_range(struct rangemap_entry *rme)
{
	struct rb_node **new, *parent;
	if (find_insertion_point(rme->pfn_start, rme->pfn_end, rme->nid, &new, &parent))
		return 1;

	rb_link_node(&rme->node, parent, new);
	rb_insert_color(&rme->node, &pfn_to_node_map);
	return 0;
}

bool ml_is_empty(struct memlayout *ml)
{
	return ml->root.node == NULL;
}
#endif

int memlayout_new_range(struct memlayout *ml, unsigned long pfn_start, unsigned long pfn_end, int nid)
{
	struct rb_node **new, *parent;
	struct rangemap_entry *rme;
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

int memlayout_pfn_to_nid_no_pageflags(unsigned long pfn)
{
	struct rb_node *node;
	struct memlayout *ml;
	rcu_read_lock();
	ml = rcu_dereference(pfn_to_node_map);
	if (!ml)
		goto out;
	node = ml->root.rb_node;
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

out:
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

void memlayout_commit_initial(struct memlayout *ml)
{
	struct memlayout *old_ml;
	ml_update_lock();
	old_ml = rcu_dereference_protected(pfn_to_node_map, ml_update_is_locked());
	if (WARN(old_ml, "memlayout_commit_initial is not first: ")) {
		ml_update_unlock();
		ml_destroy(ml);
	} else {
		rcu_assign_pointer(pfn_to_node_map, ml);
		ml_update_unlock();
		synchronize_rcu();
	}
}

void ml_destroy(struct memlayout *ml)
{
	free_rme_tree(ml->root.rb_node);
	kfree(ml);
}

struct memlayout *ml_create(void)
{
	struct memlayout *ml = kmalloc(sizeof(*ml), GFP_KERNEL);
	if (!ml)
		return NULL;

	ml->root.rb_node = NULL;
	ml_dbgfs_init(ml);
	return ml;
}

void memlayout_commit(struct memlayout *ml)
{
	struct memlayout *old_ml;

	ml_update_lock();
	ml_dbgfs_set_current(ml);
	old_ml = rcu_dereference_protected(pfn_to_node_map,
			ml_update_is_locked());
	rcu_assign_pointer(pfn_to_node_map, ml);
	ml_update_unlock();

	synchronize_rcu();

	ml_destroy(old_ml);
}

int __init_memblock memlayout_init_from_memblock(void)
{
	int i, nid, errs = 0;
	unsigned long start, end;
	struct memlayout *ml = ml_create();
	if (WARN_ON(!ml))
		return -ENOMEM;
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, &nid) {
		int r = memlayout_new_range(ml, start, end - 1, nid);
		if (r) {
			pr_err("failed to add range [%lx, %lx] in node %d to mapping",
					start, end, nid);
			errs++;
		} else
			pr_devel("added ranged [%lx, %lx] in node %d",
					start, end, nid);
	}

	memlayout_commit_initial(ml);
	return errs;
}
