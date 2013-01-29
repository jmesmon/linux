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
	ML_DNUMA,
	ML_NUM_TYPES
};

/*
 * - rbtree of {node, start, end}.
 * - assumes no 'ranges' overlap.
 */
struct rangemap_entry {
	struct rb_node node;
	unsigned long pfn_start;
	/* @pfn_end: inclusive, not stored as a count to make the lookup
	 *           faster
	 */
	unsigned long pfn_end;
	int nid;
};

struct memlayout {
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
	unsigned seq;
	struct dentry *d;
#endif
};

extern __rcu struct memlayout *pfn_to_node_map;

/* FIXME: overflow potential in completion check */
#define ml_for_each_pfn_in_range(rme, pfn)	\
	for (pfn = rme->pfn_start;		\
	     pfn <= rme->pfn_end;		\
	     pfn++)

#define ml_for_each_range(ml, rme) \
	for (rme = rb_entry(rb_first(&ml->root), typeof(*rme), node);	\
	     &rme->node;						\
	     rme = rb_entry(rb_next(&rme->node), typeof(*rme), node))

struct memlayout *memlayout_create(enum memlayout_type);
void              memlayout_destroy(struct memlayout *ml);

/* Callers accesing the same memlayout are assumed to be serialized */
int memlayout_new_range(struct memlayout *ml,
		unsigned long pfn_start, unsigned long pfn_end, int nid);

/* only queries the memlayout tracking structures. */
int memlayout_pfn_to_nid(unsigned long pfn);

/* Put ranges added by memlayout_new_range() into use by
 * memlayout_pfn_get_nid() and retire old ranges.
 *
 * No modifications to a memlayout can be made after it is commited.
 *
 * sleeps via syncronize_rcu().
 *
 * memlayout takes ownership of ml, no futher mamlayout_new_range's should be
 * issued
 */
void memlayout_commit(struct memlayout *ml);

/* Sets up an inital memlayout in early boot.
 * A weak default which uses memblock is provided.
 */
void memlayout_global_init(void);

#else /* ! defined(CONFIG_DYNAMIC_NUMA) */

/* memlayout_new_range() & memlayout_commit() are purposefully omitted */

static inline void memlayout_global_init(void)
{}

static inline int memlayout_pfn_to_nid(unsigned long pfn)
{
	return NUMA_NO_NODE;
}
#endif /* !defined(CONFIG_DYNAMIC_NUMA) */

#endif
