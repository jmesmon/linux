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

#if 0
/* must keep 'range_ct' from memlayout */
struct pfn_iterator {
	atomic_t ref_ct;
	struct flex_array *ranges;
};
#endif

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

/* XXX: add nodemask for use in dnuma_online_required_nodes & build in
 * memlayout_new_range()? */
struct memlayout {
	struct rb_root root;
	enum memlayout_type type;
	struct rangemap_entry *cache;
#if 0
	unsigned long range_ct;
	struct pfn_iterator *pfn_iterator;
#endif
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
int memlayout_pfn_to_nid_no_pageflags(unsigned long pfn);

/* Put ranges added by memlayout_new_range() into use by
 * memlayout_pfn_get_nid() and retire old ranges.
 * Any subsequent uses of memlayout_new_range() begin to build a new range map.
 *
 * sleeps via syncronize_rcu().
 *
 * memlayout takes ownership of ml, no futher mamlayout_new_range's should be
 * issued
 */
void memlayout_commit(struct memlayout *ml);

/* only commits the changes if it is the first to do so. Otherwise makes no
 * changes to the memlayout.
 * For use in arch_memlayout_init() */
void memlayout_commit_initial(struct memlayout *ml);

/* reqires: slab_is_available()
 * sleeps: definitely: memlayout_commit() -> synchronize_rcu()
 *	   potentially: kmalloc()
 */
int memlayout_init_from_memblock(void) __init_memblock;

/* Sets up an inital memlayout in early boot.
 * A weak default which uses memblock is provided.
 */
void memlayout_global_init(void);

#else /* ! defined(CONFIG_DYNAMIC_NUMA) */

/* memlayout_new_range() & memlayout_commit*() are purposefully omitted */
static inline void memlayout_global_init(void)
{}
static inline int memlayout_pfn_to_nid_no_pageflags(unsigned long pfn)
{ return NUMA_NO_NODE; }
static inline void arch_memlayout_init(void)
{}
static inline int __init_memblock memlayout_init_from_memblock(void)
{ return 0; }
#endif

/* falls back on pfn_to_nid() [using pageflags] when memlayout tracking doesn't
 * know what node the pfn belongs to. */
static inline int memlayout_pfn_to_nid(unsigned long pfn)
{
	int nid = memlayout_pfn_to_nid_no_pageflags(pfn);
	if (nid == NUMA_NO_NODE)
		nid = pfn_to_nid(pfn);
	return nid;
}

static inline int memlayout_page_to_nid(struct page *page)
{
	return memlayout_pfn_to_nid(page_to_pfn(page));
}

static inline struct zone *memlayout_page_zone(struct page *page)
{
	return &NODE_DATA(memlayout_page_to_nid(page))->node_zones[page_zonenum(page)];
}


#endif
