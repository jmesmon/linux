#ifndef LINUX_MEMLAYOUT_H_
#define LINUX_MEMLAYOUT_H_

#include <linux/memblock.h> /* __init_memblock */
#include <linux/mm.h>       /* NODE_DATA, page_zonenum */
#include <linux/mmzone.h>   /* pfn_to_nid */
#include <linux/rbtree.h>
#include <linux/types.h>    /* size_t */

#ifdef CONFIG_DYNAMIC_NUMA
# ifdef NODE_NOT_IN_PAGE_FLAGS
#  error "CONFIG_DYNAMIC_NUMA requires the NODE is in page flags. Try freeing up some flags by decreasing the maximum number of NUMA nodes, or switch to sparsmem-vmemmap"
# endif

enum memlayout_type {
	ML_INITIAL,
	ML_USER_DEBUG,
	ML_NUM_TYPES
};

struct rangemap_entry {
	struct rb_node node;
	unsigned long pfn_start;
	/* @pfn_end: inclusive, not stored as a count to make the lookup
	 *           faster
	 */
	unsigned long pfn_end;
	int nid;
};

enum memlayout_stat {
	MLSTAT_CACHE_HIT,
	MLSTAT_CACHE_MISS,
	MLSTAT_TRANSPLANT_ON_FREE,
	MLSTAT_TRANSPLANT_FROM_FREELIST_ADD,

	MLSTAT_ZONELIST_REBUILD,
	MLSTAT_NO_ZONELIST_REBUILD,
	MLSTAT_PCP_SETUP,
	MLSTAT_PCP_UPDATE,

	MLSTAT_PCP_DRAIN,
	MLSTAT_SPLIT_PAGES,

	MLSTAT_TRANSPLANT_BAIL_RESERVED,
	MLSTAT_TRANSPLANT_BAIL_NID_EQ,
	MLSTAT_TRANSPLANT_BAIL_PAGE_NOT_BUDDY,
	MLSTAT_TRANSPLANT_BAIL_ALREADY_DONE,

	MLSTAT_TRANSPLANT_FROM_FREELIST_REMOVE,
	MLSTAT_TRANSPLANT_EXAMINED_PFN,
	MLSTAT_DRAIN_ZONESTAT,

	MLSTAT_FUTURE_ZONE_FIXUP,

	MLSTAT_COUNT
};

#define RME_FMT "{%05lx-%05lx}:%d"
#define RME_EXP(rme) rme->pfn_start, rme->pfn_end, rme->nid

struct memlayout {
	/*
	 * - contains rangemap_entrys.
	 * - assumes no 'ranges' overlap.
	 */
	struct rb_root root;
	enum memlayout_type type;

	/*
	 * When a memlayout is commited, 'cache' is accessed (the field is read
	 * from & written to) by multiple tasks without additional locking
	 * (other than the rcu locking for accessing the memlayout).
	 *
	 * Do not assume that it will not change. Use ACCESS_ONCE() to avoid
	 * potential races.
	 */
	struct rangemap_entry *cache;

#ifdef CONFIG_DNUMA_DEBUGFS
	struct list_head list;
	unsigned seq;
	struct dentry *d;

	/*
	 * XXX: This is rather large. Consider: allow building with debugfs
	 * enabled and allow stat collection to be runtime enabled, and/or
	 * allow building the debugfs interface as a module, and/or switch to
	 * tracepoints.
	 */
	atomic64_t stats[MLSTAT_COUNT];
	atomic64_t node_stats[MAX_NUMNODES][MLSTAT_COUNT];
#endif
};

/*** Global memlayout (pfn_to_node_map) operations */

extern __rcu struct memlayout *pfn_to_node_map;
extern struct mutex memlayout_lock; /* update-side lock */

static inline struct memlayout *memlayout_rcu_deref_if_active(void)
{
	struct memlayout *ml = rcu_dereference(pfn_to_node_map);
	if (ml && (ml->type != ML_INITIAL))
		return ml;
	return NULL;
}

int memlayout_pfn_to_nid(unsigned long pfn);
int memlayout_pfn_to_nid_if_active(unsigned long pfn);

/*** Generic memlayout operations */

#define ml_for_each_pfn_in_range(rme, pfn)	\
	for (pfn = rme->pfn_start;		\
	     pfn <= rme->pfn_end || pfn < rme->pfn_start; \
	     pfn++)

static inline bool rme_bounds_pfn(struct rangemap_entry *rme, unsigned long pfn)
{
	return rme->pfn_start <= pfn && pfn <= rme->pfn_end;
}

static inline struct rangemap_entry *rme_next(struct rangemap_entry *rme)
{
	struct rb_node *node = rb_next(&rme->node);
	if (!node)
		return NULL;
	return rb_entry(node, typeof(*rme), node);
}

static inline struct rangemap_entry *rme_first(struct memlayout *ml)
{
	struct rb_node *node = rb_first(&ml->root);
	if (!node)
		return NULL;
	return rb_entry(node, struct rangemap_entry, node);
}

#define ml_for_each_range(ml, rme) \
	for (rme = rme_first(ml);	\
	     &rme->node;		\
	     rme = rme_next(rme))

struct memlayout *memlayout_create(enum memlayout_type);

static inline bool memlayout_exists(void)
{
	return !!rcu_access_pointer(pfn_to_node_map);
}

/*
 * In most cases, these should only be used by the memlayout debugfs code (or
 * internally within memlayout)
 */
void memlayout_destroy(struct memlayout *ml);
void memlayout_destroy_mem(struct memlayout *ml);

int memlayout_new_range(struct memlayout *ml,
		unsigned long pfn_start, unsigned long pfn_end, int nid);

struct rangemap_entry *memlayout_pfn_to_rme(struct memlayout *ml,
					    unsigned long pfn);
int _memlayout_pfn_to_nid(struct memlayout *ml, unsigned long pfn);

/*
 * Put ranges added by memlayout_new_range() into use by
 * memlayout_pfn_get_nid() and retire old memlayout.
 *
 * No modifications to a memlayout should be made after it is commited.
 */
void memlayout_commit(struct memlayout *ml);

/*
 * Set up an inital memlayout in early boot.
 * A weak default which uses memblock is provided.
 */
void memlayout_global_init(void);

#else /* !defined(CONFIG_DYNAMIC_NUMA) */

/* memlayout_new_range() & memlayout_commit() are purposefully omitted */

static inline void memlayout_global_init(void)
{}

static inline int memlayout_pfn_to_nid(unsigned long pfn)
{
	return NUMA_NO_NODE;
}

static inline int memlayout_pfn_to_nid_if_active(unsigned long pfn)
{
	return NUMA_NO_NODE;
}
#endif /* !defined(CONFIG_DYNAMIC_NUMA) */

#endif
