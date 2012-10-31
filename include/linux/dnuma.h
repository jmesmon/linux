#ifndef DNUMA_H_
#define DNUMA_H_

#include <linux/memblock.h> /* __init_memblock */
#include <linux/types.h>    /* size_t */
#include <linux/mm.h>       /* NODE_DATA, page_zonenum */

#ifdef CONFIG_DYNAMIC_NUMA
# ifdef NODE_NOT_IN_PAGE_FLAGS
#  error "CONFIG_DYNAMIC_NUMA requires the NODE is in page flags. Try freeing up some flags by decreasing the maximum number of NUMA nodes, or switch to sparsmem-vmemmap"
# endif

/* Callers are assumed to be serialized */
int memlayout_new_range(unsigned long pfn_start, unsigned long pfn_end, int nid);

/* only queries the memlayout tracking structures. */
int memlayout_pfn_to_nid_no_pageflags(unsigned long pfn);

/* Put ranges added by memlayout_new_range() into use by
 * memlayout_pfn_get_nid() and retire old ranges.
 * Any subsequent uses of memlayout_new_range() begin to build a new range map.
 *
 * sleeps via syncronize_rcu().
 */
void memlayout_commit(void);

/* sleeps - calls memlayout_sync() */
int memlayout_init_from_memblock(void) __init_memblock;
void arch_memlayout_init(void); /* defined in an architecture specific file */
#else /* ! defined(CONFIG_DYNAMIC_NUMA) */

static inline int memlayout_new_range(unsigned long pfn_start, unsigned long pfn_end, int nid)
{ return 0; }
static inline int memlayout_pfn_to_nid_no_pageflags(unsigned long pfn)
{ return NUMA_NO_NODE; }
static inline void memlayout_commit(void) {}

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
	return &NODE_DATA(memlayout_page_to_nid(page))->node_zones[page_zonenum(page)];
}

static inline struct zone *memlayout_notify_page_is_being_freed(struct page *page)
{
	/* XXX: fixup the old zone & new zone's present_pages & spanned_pages & zone_start_pfn */
	/* use span_seqlock */
	/* XXX: should this be done at the time when we recieve knowledge of
	 * the new layout, or when the page's zone is actually changed? */
	return NULL;
}

#endif
