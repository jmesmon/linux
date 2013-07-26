
#include <linux/debugfs.h>
#include <linux/kernel.h> /* CLAMP_MIN */
#include <linux/list.h>
#include <linux/log2.h> /* roundup_pow_of_two */
#include <linux/module.h> /* THIS_MODULE, needed for DEFINE_SIMPLE_ATTR */
#include <linux/slab.h> /* kmalloc */

#include "memlayout-debugfs.h"

static const char *ml_stat_names[] = {
	"cache-hit",
	"cache-miss",
	"transplant-on-free",
	"transplant-from-freelist-add",

	"zonelist-rebuild",
	"no-zonelist-rebuild",
	"pcp-setup",
	"pcp-update",

	"pcp-drain",
	"split-pages",

	"transplant-bail-reserved",
	"transplant-bail-nid-eq",
	"transplant-bail-page-not-buddy",
	"transplant-bail-already-done",

	"transplant-from-freelist-remove",
	"transplant-examined-pfn",
	"drain-zonestat",

	"future-zone-fixup",
};

/* nests inside of memlayout_lock */
static DEFINE_MUTEX(ml_dbgfs_lock);

/* backlog handling {{{ */
static unsigned long backlog_max = CONFIG_DNUMA_BACKLOG;
module_param(backlog_max, ulong, 0644);
static LIST_HEAD(ml_backlog);
static size_t backlog_ct;

/* memlayout_lock must be held */
void ml_backlog_feed(struct memlayout *ml)
{
	kparam_block_sysfs_write(backlog_max);

	while (backlog_ct + 1 > backlog_max) {
		struct memlayout *old_ml = list_first_entry_or_null(&ml_backlog,
						typeof(*old_ml), list);
		/*
		 * occurs when backlog_max == 0, meaning the backlog is
		 * disabled
		 */
		if (!old_ml)
			return;

		list_del(&old_ml->list);
		backlog_ct--;
		memlayout_destroy(old_ml);
	}

	if (backlog_ct + 1 < backlog_max) {
		list_add_tail(&ml->list, &ml_backlog);
		backlog_ct++;
	} else
		memlayout_destroy(ml);

	kparam_unblock_sysfs_write(backlog_max);
}
/* }}} */

static atomic_t ml_seq = ATOMIC_INIT(0);
static struct dentry *root_dentry, *current_dentry;
#define ML_LAYOUT_NAME_SZ \
	((size_t)(DIV_ROUND_UP(sizeof(unsigned) * 8, 3) \
				+ 1 + strlen("layout.")))
#define ML_RANGE_NAME_SZ ((size_t)(2 * BITS_PER_LONG / 4 + 2))

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
static void _ml_dbgfs_set_current(struct memlayout *ml, char *name_buf)
{
	ml_layout_name(ml, name_buf);
	debugfs_remove(current_dentry);
	current_dentry = debugfs_create_symlink("current", root_dentry,
						name_buf);
}

static atomic64_t ml_stats[MLSTAT_COUNT];
/* XXX: add to NODE_DATA()? */
static atomic64_t ml_node_stats[MAX_NUMNODES][MLSTAT_COUNT];

void ml_stat_add(enum memlayout_stat stat, struct memlayout *ml, int node, int order)
{
	atomic64_add(1 << order, &ml_stats[stat]);
	if (node != NUMA_NO_NODE)
		atomic64_add(1 << order, &ml_node_stats[node][stat]);
	if (ml) {
		atomic64_add(1 << order, &ml->stats[stat]);
		if (node != NUMA_NO_NODE)
			atomic64_add(1 << order, &ml->node_stats[node][stat]);
	}
}

void ml_stat_inc(enum memlayout_stat stat, struct memlayout *ml, int node)
{
	atomic64_inc(&ml_stats[stat]);
	if (node != NUMA_NO_NODE)
		atomic64_inc(&ml_node_stats[node][stat]);
	if (ml) {
		atomic64_inc(&ml->stats[stat]);
		if (node != NUMA_NO_NODE)
			atomic64_inc(&ml->node_stats[node][stat]);
	}
}

#define UNSIGNED_DIGIT_CT ((size_t)DIV_ROUND_UP(sizeof(unsigned) * 8, 3))

static void create_stats_under(struct dentry *d, atomic64_t *stat_buf)
{
	size_t i;
	for (i = 0; i < MLSTAT_COUNT; i++)
		debugfs_create_atomic_u64(ml_stat_names[i], 0400, d, &stat_buf[i]);
}

static void create_node_stats_under(struct dentry *d,
		atomic64_t node_stat_buf[][MLSTAT_COUNT])
{
	char node_buf[UNSIGNED_DIGIT_CT + 1];
	int i;
	for (i = 0; i < nr_node_ids; i++) {
		struct dentry *nd;
		sprintf(node_buf, "%d", i);
		nd = debugfs_create_dir(node_buf, d);
		if (WARN_ON(!nd))
			return;

		create_stats_under(nd, node_stat_buf[i]);
	}
}

static void create_stat_dirs(struct dentry *top_d, atomic64_t stat_buf[],
		atomic64_t node_stat_buf[][MLSTAT_COUNT])
{
	struct dentry *d = debugfs_create_dir("stats", top_d);
	if (WARN_ON(!d))
		return;

	create_stats_under(d, stat_buf);

	d = debugfs_create_dir("node_stats", top_d);
	if (WARN_ON(!d))
		return;

	create_node_stats_under(d, node_stat_buf);
}

static void create_global_stats(void)
{
	create_stat_dirs(root_dentry, ml_stats, ml_node_stats);
}

static void create_ml_stats(struct memlayout *ml)
{
	create_stat_dirs(ml->d, ml->stats, ml->node_stats);
}

static void ml_dbgfs_create_layout_dir_assume_root(struct memlayout *ml)
{
	char name[ML_LAYOUT_NAME_SZ];
	ml_layout_name(ml, name);
	WARN_ON(!root_dentry);
	ml->d = debugfs_create_dir(name, root_dentry);
	WARN_ON(!ml->d);
	create_ml_stats(ml);
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

/* @name_buf must be at least ML_RANGE_NAME_SZ bytes */
static int ml_dbgfs_memlayout_create_layout(struct memlayout *ml,
					    char *name_buf)
{
	struct rangemap_entry *rme;
	ml_dbgfs_create_layout_dir_assume_root(ml);
	if (!ml->d)
		return -1;

	ml_for_each_range(ml, rme)
		_ml_dbgfs_create_range(ml->d, rme, name_buf);

	return 0;
}

/* create the entire current memlayout.
 * only used for the layout which exsists prior to fs initialization
 */
static void ml_dbgfs_create_layout_current(void)
{
	char name_buf[max(ML_RANGE_NAME_SZ, ML_LAYOUT_NAME_SZ)];
	struct memlayout *ml;

	ml = rcu_dereference_protected(pfn_to_node_map,
			mutex_is_locked(&memlayout_lock));

	if (!ml_dbgfs_memlayout_create_layout(ml, name_buf))
		_ml_dbgfs_set_current(ml, name_buf);
}

static void ml_dbgfs_create_layouts_defered(void)
{
	struct memlayout *ml;
	char name_buf[ML_RANGE_NAME_SZ];
	list_for_each_entry(ml, &ml_backlog, list)
		ml_dbgfs_memlayout_create_layout(ml, name_buf);
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

	create_global_stats();

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

	return 0;
}

static void ml_dbgfs_create_layout_dir(struct memlayout *ml)
{
	if (ml_dbgfs_create_root()) {
		ml->d = NULL;
		return;
	}
	ml_dbgfs_create_layout_dir_assume_root(ml);
}

/* Interface */
void ml_dbgfs_memlayout_init(struct memlayout *ml)
{
	mutex_lock(&ml_dbgfs_lock);
	ml->seq = atomic_inc_return(&ml_seq) - 1;
	ml_dbgfs_create_layout_dir(ml);
	memset(ml->stats, 0, sizeof(ml->stats));
	memset(ml->node_stats, 0, sizeof(ml->node_stats));
	mutex_unlock(&ml_dbgfs_lock);
}

void ml_dbgfs_memlayout_create_range(struct memlayout *ml,
				     struct rangemap_entry *rme)
{
	char name[ML_RANGE_NAME_SZ];
	mutex_lock(&ml_dbgfs_lock);
	if (ml->d)
		_ml_dbgfs_create_range(ml->d, rme, name);
	mutex_unlock(&ml_dbgfs_lock);
}

void ml_dbgfs_set_current(struct memlayout *ml)
{
	char name[ML_LAYOUT_NAME_SZ];
	mutex_lock(&ml_dbgfs_lock);
	_ml_dbgfs_set_current(ml, name);
	mutex_unlock(&ml_dbgfs_lock);
}

void ml_dbgfs_memlayout_dini(struct memlayout *ml)
{
	mutex_lock(&ml_dbgfs_lock);
	if (ml && ml->d)
		debugfs_remove_recursive(ml->d);
	mutex_unlock(&ml_dbgfs_lock);
}

static int ml_dbgfs_create(void)
{
	/*
	 * If you trigger this, make sure ml_stat_names[] (at the top of this
	 * file) has an entry for the new enum memlayout_stat (MLSTAT_*) entry
	 * you added.
	 */
	BUILD_BUG_ON(ARRAY_SIZE(ml_stat_names) != MLSTAT_COUNT);

	mutex_lock(&memlayout_lock);
	mutex_lock(&ml_dbgfs_lock);

	ml_dbgfs_create_root();
	ml_dbgfs_create_layout_current();
	mutex_unlock(&memlayout_lock);

	ml_dbgfs_create_layouts_defered();

	mutex_unlock(&ml_dbgfs_lock);
	return 0;
}
module_init(ml_dbgfs_create);

static void __exit ml_dbgfs_destroy(void)
{
	mutex_lock(&ml_dbgfs_lock);
	debugfs_remove_recursive(root_dentry);
	root_dentry = NULL;
	mutex_unlock(&ml_dbgfs_lock);
}
module_exit(ml_dbgfs_destroy);
