#ifndef LINUX_MEMLAYOUT_H_
#define LINUX_MEMLAYOUT_H_
#define DEBUG 1

#include <linux/memblock.h> /* __init_memblock */
#include <linux/types.h>    /* size_t */
#include <linux/mm.h>       /* NODE_DATA, page_zonenum */
#include <linux/mmzone.h>   /* pfn_to_nid */

#ifdef CONFIG_DYNAMIC_NUMA
# ifdef NODE_NOT_IN_PAGE_FLAGS
#  error "CONFIG_DYNAMIC_NUMA requires the NODE is in page flags. Try freeing up some flags by decreasing the maximum number of NUMA nodes, or switch to sparsmem-vmemmap"
# endif

struct memlayout {
	struct rb_root root;
#ifdef CONFIG_DEBUG_FS
	unsigned seq;
	struct dentry *d;
#endif
};

#ifndef CONFIG_DEBUG_FS
#define MEMLAYOUT_INIT { RB_ROOT }
#define ml_init(ml) do { \
	ml->root = RB_ROOT;
} while (0)
#else
#define MEMLAYOUT_INIT { RB_ROOT, UINT_MAX, NULL}
#define ml_init(ml) do { \
	ml->seq = UINT_MAX;
	ml->d = NULL;
	ml->root = RB_ROOT;
} while(0)
#endif

#define DEFINE_MEMLAYOUT(name) struct memlayout name = MEMLAYOUT_INIT

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
 * memlayout takes ownership of ml, no futher mamlayout_new_range's should be issued
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

/* defined in an arch code. Called in init/main.c following kmem_cache
 * initialization. */
void arch_memlayout_init(void);

static inline struct zone *get_zone(int nid, enum zone_type zonenum)
{
	return &NODE_DATA(nid)->node_zones[zonenum];
}

#else /* ! defined(CONFIG_DYNAMIC_NUMA) */

/* memlayout_new_range() & memlayout_commit*() are purposefully omitted */
static inline int memlayout_pfn_to_nid_no_pageflags(unsigned long pfn)
{ return NUMA_NO_NODE; }

static inline void arch_memlayout_init(void) {}
static inline int __init_memblock memlayout_init_from_memblock(void) { return 0; }
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
#if 0
	int nid = memlayout_page_to_nid(page);
	pg_data_t *pg = NODE_DATA(nid);
	enum zone_type zonenum = page_zonenum(page);
	struct zone *zone = &pg->node_zones[zonenum];
	return zone;
#else
	return &NODE_DATA(memlayout_page_to_nid(page))->node_zones[page_zonenum(page)];
#endif
}


#endif
