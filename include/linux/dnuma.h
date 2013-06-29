#ifndef LINUX_DNUMA_H_
#define LINUX_DNUMA_H_

#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/memlayout.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/slab.h>

#ifdef CONFIG_DYNAMIC_NUMA
/* Must be called _before_ setting a new_ml to the pfn_to_node_map */
int dnuma_online_required_nodes_and_zones(struct memlayout *old_ml,
		struct memlayout *new_ml);

/* Must be called _after_ setting a new_ml to the pfn_to_node_map */
void dnuma_move_free_pages(struct memlayout *old_ml, struct memlayout *new_ml);

static inline void lookup_node_flags_free(struct mem_section *ms)
{
	unsigned long *lnm = ms->lookup_node_mark;
	ms->lookup_node_mark = NULL;
	kfree(lnm);
}

static inline bool lookup_node_test_clear_pfn(unsigned long pfn)
{
	unsigned long first_pfn_in_sec = SECTION_ALIGN_DOWN(pfn);
	struct mem_section *ms = __pfn_to_section(pfn);
	if (!ms->lookup_node_mark)
		return false;

	return test_and_clear_bit(pfn - first_pfn_in_sec, ms->lookup_node_mark);
}

int dnuma_page_needs_move_lookup(struct page *page);

static inline int dnuma_page_needs_move(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);

	if (!lookup_node_test_clear_pfn(pfn))
		return NUMA_NO_NODE;

	return dnuma_page_needs_move_lookup(page);
}

void dnuma_add_page_to_new_zone(struct page *page, int order,
				struct zone *dest_zone,
				int dest_nid);

#else /* !defined CONFIG_DYNAMIC_NUMA */
static inline void lookup_node_flags_free(struct mem_section *ms)
{}

static inline void dnuma_add_page_to_new_zone(struct page *page, int order,
				struct zone *dest_zone,
				int dest_nid)
{
	BUG();
}

static inline int dnuma_page_needs_move(struct page *page)
{
	return NUMA_NO_NODE;
}
#endif /* !defined CONFIG_DYNAMIC_NUMA */

#endif /* defined LINUX_DNUMA_H_ */
