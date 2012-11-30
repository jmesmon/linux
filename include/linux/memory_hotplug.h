#ifndef __LINUX_MEMORY_HOTPLUG_H
#define __LINUX_MEMORY_HOTPLUG_H

#include <linux/mmzone.h>
#include <linux/notifier.h>
#include <linux/dynamic-nodes.h>

struct page;
struct zone;
struct pglist_data;
struct mem_section;
struct memory_block;

#ifdef CONFIG_MEMORY_HOTPLUG

/*
 * Types for free bootmem stored in page->lru.next. These have to be in
 * some random range in unsigned long space for debugging purposes.
 */
enum {
	MEMORY_HOTPLUG_MIN_BOOTMEM_TYPE = 12,
	SECTION_INFO = MEMORY_HOTPLUG_MIN_BOOTMEM_TYPE,
	MIX_SECTION_INFO,
	NODE_INFO,
	MEMORY_HOTPLUG_MAX_BOOTMEM_TYPE = NODE_INFO,
};

extern int zone_grow_free_lists(struct zone *zone, unsigned long new_nr_pages);
extern int zone_grow_waitqueues(struct zone *zone, unsigned long nr_pages);
extern int add_one_highpage(struct page *page, int pfn, int bad_ppro);
/* VM interface that may be used by firmware interface */
extern int online_pages(unsigned long, unsigned long);
extern void __offline_isolated_pages(unsigned long, unsigned long);

typedef void (*online_page_callback_t)(struct page *page);

extern int set_online_page_callback(online_page_callback_t callback);
extern int restore_online_page_callback(online_page_callback_t callback);

extern void __online_page_set_limits(struct page *page);
extern void __online_page_increment_counters(struct page *page);
extern void __online_page_free(struct page *page);

#ifdef CONFIG_MEMORY_HOTREMOVE
extern bool is_pageblock_removable_nolock(struct page *page);
#endif /* CONFIG_MEMORY_HOTREMOVE */

/* reasonably generic interface to expand the physical pages in a zone  */
extern int __add_pages(int nid, struct zone *zone, unsigned long start_pfn,
	unsigned long nr_pages);
extern int __remove_pages(struct zone *zone, unsigned long start_pfn,
	unsigned long nr_pages);

#ifdef CONFIG_NUMA
extern int memory_add_physaddr_to_nid(u64 start);
#else
static inline int memory_add_physaddr_to_nid(u64 start)
{
	return 0;
}
#endif


#ifdef CONFIG_SPARSEMEM_VMEMMAP
static inline void register_page_bootmem_info_node(struct pglist_data *pgdat)
{
}
static inline void put_page_bootmem(struct page *page)
{
}
#else
extern void register_page_bootmem_info_node(struct pglist_data *pgdat);
extern void put_page_bootmem(struct page *page);
#endif

/*
 * Lock for memory hotplug guarantees 1) all callbacks for memory hotplug
 * notifier will be called under this. 2) offline/online/add/remove memory
 * will not run simultaneously.
 */

void lock_memory_hotplug(void);
void unlock_memory_hotplug(void);

#else /* ! CONFIG_MEMORY_HOTPLUG */
/*
 * Stub functions for when hotplug is off
 */

static inline int mhp_notimplemented(const char *func)
{
	printk(KERN_WARNING "%s() called, with CONFIG_MEMORY_HOTPLUG disabled\n", func);
	dump_stack();
	return -ENOSYS;
}

static inline void register_page_bootmem_info_node(struct pglist_data *pgdat)
{
}

static inline void lock_memory_hotplug(void) {}
static inline void unlock_memory_hotplug(void) {}

#endif /* ! CONFIG_MEMORY_HOTPLUG */

#ifdef CONFIG_MEMORY_HOTREMOVE

extern int is_mem_section_removable(unsigned long pfn, unsigned long nr_pages);

#else
static inline int is_mem_section_removable(unsigned long pfn,
					unsigned long nr_pages)
{
	return 0;
}
#endif /* CONFIG_MEMORY_HOTREMOVE */

extern int mem_online_node(int nid);
extern int add_memory(int nid, u64 start, u64 size);
extern int arch_add_memory(int nid, u64 start, u64 size);
extern int offline_pages(unsigned long start_pfn, unsigned long nr_pages);
extern int offline_memory_block(struct memory_block *mem);
extern int remove_memory(u64 start, u64 size);
extern int sparse_add_one_section(struct zone *zone, unsigned long start_pfn,
								int nr_pages);
extern void sparse_remove_one_section(struct zone *zone, struct mem_section *ms);
extern struct page *sparse_decode_mem_map(unsigned long coded_mem_map,
					  unsigned long pnum);

#endif /* __LINUX_MEMORY_HOTPLUG_H */
