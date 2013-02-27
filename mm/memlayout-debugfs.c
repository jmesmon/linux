#include <linux/debugfs.h>

#include <linux/slab.h> /* kmalloc */
#include <linux/module.h> /* THIS_MODULE, needed for DEFINE_SIMPLE_ATTR */

#include "memlayout-debugfs.h"

#if CONFIG_DNUMA_BACKLOG > 0
/* Fixed size backlog */
#include <linux/kfifo.h>
#include <linux/log2.h> /* roundup_pow_of_two */
#include <linux/kernel.h> /* clamp */
DEFINE_KFIFO(ml_backlog, struct memlayout *,
		CLAMP_MIN(roundup_pow_of_two(CONFIG_DNUMA_BACKLOG), 2));
void ml_backlog_feed(struct memlayout *ml)
{
	if (kfifo_is_full(&ml_backlog)) {
		struct memlayout *old_ml;
		BUG_ON(!kfifo_get(&ml_backlog, &old_ml));
		memlayout_destroy(old_ml);
	}

	kfifo_put(&ml_backlog, (const struct memlayout **)&ml);
}
#elif CONFIG_DNUMA_BACKLOG < 0
/* Unlimited backlog */
void ml_backlog_feed(struct memlayout *ml)
{
	/* we never use the rme_tree, so we destroy the non-debugfs portions to
	 * save memory */
	memlayout_destroy_mem(ml);
}
#else /* CONFIG_DNUMA_BACKLOG == 0 */
/* No backlog */
void ml_backlog_feed(struct memlayout *ml)
{
	memlayout_destroy(ml);
}
#endif

static atomic64_t dnuma_moved_page_ct;
void ml_stat_count_moved_pages(int order)
{
	atomic64_add(1 << order, &dnuma_moved_page_ct);
}

static atomic_t ml_seq = ATOMIC_INIT(0);
static struct dentry *root_dentry, *current_dentry;
#define ML_LAYOUT_NAME_SZ \
	((size_t)(DIV_ROUND_UP(sizeof(unsigned) * 8, 3) \
				+ 1 + strlen("layout.")))
#define ML_REGION_NAME_SZ ((size_t)(2 * BITS_PER_LONG / 4 + 2))

static void ml_layout_name(struct memlayout *ml, char *name)
{
	sprintf(name, "layout.%u", ml->seq);
}

static int dfs_range_get(void *data, u64 *val)
{
	*val = (uintptr_t)data;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(range_fops, dfs_range_get, NULL, "%lld\n");

static void _ml_dbgfs_create_range(struct dentry *base,
		struct rangemap_entry *rme, char *name)
{
	struct dentry *rd;
	sprintf(name, "%05lx-%05lx", rme->pfn_start, rme->pfn_end);
	rd = debugfs_create_file(name, 0400, base,
				(void *)(uintptr_t)rme->nid, &range_fops);
	if (!rd)
		pr_devel("debugfs: failed to create "RME_FMT"\n",
				RME_EXP(rme));
	else
		pr_devel("debugfs: created "RME_FMT"\n", RME_EXP(rme));
}

/* Must be called with memlayout_lock held */
static void _ml_dbgfs_set_current(struct memlayout *ml, char *name)
{
	ml_layout_name(ml, name);
	debugfs_remove(current_dentry);
	current_dentry = debugfs_create_symlink("current", root_dentry, name);
}

static void ml_dbgfs_create_layout_assume_root(struct memlayout *ml)
{
	char name[ML_LAYOUT_NAME_SZ];
	ml_layout_name(ml, name);
	WARN_ON(!root_dentry);
	ml->d = debugfs_create_dir(name, root_dentry);
	WARN_ON(!ml->d);
}

# if defined(CONFIG_DNUMA_DEBUGFS_WRITE)

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

#define DEFINE_ACTION_ATTR(___name)

static u64 dnuma_user_start;
static u64 dnuma_user_end;
static u32 dnuma_user_node; /* XXX: I don't care about this var, remove? */
static u8  dnuma_user_commit, dnuma_user_clear; /* same here */
static struct memlayout *user_ml;
static DEFINE_MUTEX(dnuma_user_lock);
static int dnuma_user_node_watch(u32 old_val, u32 new_val)
{
	int ret = 0;
	mutex_lock(&dnuma_user_lock);
	if (!user_ml)
		user_ml = memlayout_create(ML_USER_DEBUG);

	if (WARN_ON(!user_ml)) {
		ret = -ENOMEM;
		goto out;
	}

	if (new_val >= nr_node_ids) {
		ret = -EINVAL;
		goto out;
	}

	if (dnuma_user_start > dnuma_user_end) {
		ret = -EINVAL;
		goto out;
	}

	ret = memlayout_new_range(user_ml, dnuma_user_start, dnuma_user_end,
				  new_val);

	if (!ret) {
		dnuma_user_start = 0;
		dnuma_user_end = 0;
	}
out:
	mutex_unlock(&dnuma_user_lock);
	return ret;
}

static int dnuma_user_commit_watch(u8 old_val, u8 new_val)
{
	mutex_lock(&dnuma_user_lock);
	if (user_ml)
		memlayout_commit(user_ml);
	user_ml = NULL;
	mutex_unlock(&dnuma_user_lock);
	return 0;
}

static int dnuma_user_clear_watch(u8 old_val, u8 new_val)
{
	mutex_lock(&dnuma_user_lock);
	if (user_ml)
		memlayout_destroy(user_ml);
	user_ml = NULL;
	mutex_unlock(&dnuma_user_lock);
	return 0;
}

DEFINE_WATCHED_ATTR(u32, dnuma_user_node);
DEFINE_WATCHED_ATTR(u8, dnuma_user_commit);
DEFINE_WATCHED_ATTR(u8, dnuma_user_clear);
# endif /* defined(CONFIG_DNUMA_DEBUGFS_WRITE) */

/* create the entire current memlayout.
 * only used for the layout which exsists prior to fs initialization
 */
static void ml_dbgfs_create_initial_layout(void)
{
	struct rangemap_entry *rme;
	char name[max(ML_REGION_NAME_SZ, ML_LAYOUT_NAME_SZ)];
	struct memlayout *old_ml, *new_ml;

	new_ml = kmalloc(sizeof(*new_ml), GFP_KERNEL);
	if (WARN(!new_ml, "memlayout allocation failed\n"))
		return;

	mutex_lock(&memlayout_lock);

	old_ml = rcu_dereference_protected(pfn_to_node_map,
			mutex_is_locked(&memlayout_lock));
	if (WARN_ON(!old_ml))
		goto e_out;
	*new_ml = *old_ml;

	if (WARN_ON(new_ml->d))
		goto e_out;

	/* this assumption holds as ml_dbgfs_create_initial_layout() (this
	 * function) is only called by ml_dbgfs_create_root() */
	ml_dbgfs_create_layout_assume_root(new_ml);
	if (!new_ml->d)
		goto e_out;

	ml_for_each_range(new_ml, rme) {
		_ml_dbgfs_create_range(new_ml->d, rme, name);
	}

	_ml_dbgfs_set_current(new_ml, name);
	rcu_assign_pointer(pfn_to_node_map, new_ml);
	mutex_unlock(&memlayout_lock);

	synchronize_rcu();
	kfree(old_ml);
	return;
e_out:
	mutex_unlock(&memlayout_lock);
	kfree(new_ml);
}

static atomic64_t ml_cache_hits;
static atomic64_t ml_cache_misses;

void ml_stat_cache_miss(void)
{
	atomic64_inc(&ml_cache_misses);
}

void ml_stat_cache_hit(void)
{
	atomic64_inc(&ml_cache_hits);
}

/* returns 0 if root_dentry has been created */
static int ml_dbgfs_create_root(void)
{
	if (root_dentry)
		return 0;

	if (!debugfs_initialized()) {
		pr_devel("debugfs not registered or disabled.\n");
		return -EINVAL;
	}

	root_dentry = debugfs_create_dir("memlayout", NULL);
	if (!root_dentry) {
		pr_devel("root dir creation failed\n");
		return -EINVAL;
	}

	/* TODO: place in a different dir? (to keep memlayout & dnuma seperate)
	 */
	/* FIXME: use debugfs_create_atomic64() [does not yet exsist]. */
	debugfs_create_u64("moved-pages", 0400, root_dentry,
			   (uint64_t *)&dnuma_moved_page_ct.counter);
	debugfs_create_u64("pfn-lookup-cache-misses", 0400, root_dentry,
			   (uint64_t *)&ml_cache_misses.counter);
	debugfs_create_u64("pfn-lookup-cache-hits", 0400, root_dentry,
			   (uint64_t *)&ml_cache_hits.counter);

# if defined(CONFIG_DNUMA_DEBUGFS_WRITE)
	/* Set node last: on write, it adds the range. */
	debugfs_create_x64("start", 0600, root_dentry, &dnuma_user_start);
	debugfs_create_x64("end",   0600, root_dentry, &dnuma_user_end);
	debugfs_create_file("node",  0200, root_dentry,
			&dnuma_user_node, &dnuma_user_node_fops);
	debugfs_create_file("commit",  0200, root_dentry,
			&dnuma_user_commit, &dnuma_user_commit_fops);
	debugfs_create_file("clear",  0200, root_dentry,
			&dnuma_user_clear, &dnuma_user_clear_fops);
# endif

	/* uses root_dentry */
	ml_dbgfs_create_initial_layout();

	return 0;
}

static void ml_dbgfs_create_layout(struct memlayout *ml)
{
	if (ml_dbgfs_create_root()) {
		ml->d = NULL;
		return;
	}
	ml_dbgfs_create_layout_assume_root(ml);
}

static int ml_dbgfs_init_root(void)
{
	ml_dbgfs_create_root();
	return 0;
}

void ml_dbgfs_init(struct memlayout *ml)
{
	ml->seq = atomic_inc_return(&ml_seq) - 1;
	ml_dbgfs_create_layout(ml);
}

void ml_dbgfs_create_range(struct memlayout *ml, struct rangemap_entry *rme)
{
	char name[ML_REGION_NAME_SZ];
	if (ml->d)
		_ml_dbgfs_create_range(ml->d, rme, name);
}

void ml_dbgfs_set_current(struct memlayout *ml)
{
	char name[ML_LAYOUT_NAME_SZ];
	_ml_dbgfs_set_current(ml, name);
}

void ml_destroy_dbgfs(struct memlayout *ml)
{
	if (ml && ml->d)
		debugfs_remove_recursive(ml->d);
}

static void __exit ml_dbgfs_exit(void)
{
	debugfs_remove_recursive(root_dentry);
	root_dentry = NULL;
}

module_init(ml_dbgfs_init_root);
module_exit(ml_dbgfs_exit);
