#ifndef __LINUX_DYNAMIC_NODES_H
#define __LINUX_DYNAMIC_NODES_H

#include <linux/bug.h>
#include <linux/mmzone.h>
#include <linux/seqlock.h>
#include <linux/spinlock.h>

#ifdef CONFIG_DYNAMIC_NODES
void grow_pgdat_and_zone(struct zone *zone, unsigned long start_pfn,
			 unsigned long end_pfn);

void adjust_zone_present_pages(struct zone *zone, long delta);

/*
 * pgdat resizing functions
 */
static inline
void pgdat_resize_lock(struct pglist_data *pgdat, unsigned long *flags)
{
	spin_lock_irqsave(&pgdat->node_size_lock, *flags);
}
static inline
void pgdat_resize_unlock(struct pglist_data *pgdat, unsigned long *flags)
{
	spin_unlock_irqrestore(&pgdat->node_size_lock, *flags);
}
static inline
void pgdat_resize_init(struct pglist_data *pgdat)
{
	spin_lock_init(&pgdat->node_size_lock);
}
/*
 * Zone resizing functions
 */
static inline unsigned zone_span_seqbegin(struct zone *zone)
{
	return read_seqbegin(&zone->span_seqlock);
}
static inline int zone_span_seqretry(struct zone *zone, unsigned iv)
{
	return read_seqretry(&zone->span_seqlock, iv);
}
static inline void zone_span_writelock(struct zone *zone)
{
	write_seqlock(&zone->span_seqlock);
}
static inline void zone_span_writeunlock(struct zone *zone)
{
	write_sequnlock(&zone->span_seqlock);
}
static inline void zone_seqlock_init(struct zone *zone)
{
	seqlock_init(&zone->span_seqlock);
}

#ifdef CONFIG_HAVE_ARCH_NODEDATA_EXTENSION
/*
 * For supporting node-hotadd, we have to allocate a new pgdat.
 *
 * If an arch has generic style NODE_DATA(),
 * node_data[nid] = kzalloc() works well. But it depends on the architecture.
 *
 * In general, generic_alloc_nodedata() is used.
 * Now, arch_free_nodedata() is just defined for error path of node_hot_add.
 *
 */
extern pg_data_t *arch_alloc_nodedata(int nid);
extern void arch_free_nodedata(pg_data_t *pgdat);
extern void arch_refresh_nodedata(int nid, pg_data_t *pgdat);

#else /* CONFIG_HAVE_ARCH_NODEDATA_EXTENSION */

#define arch_alloc_nodedata(nid)	generic_alloc_nodedata(nid)
#define arch_free_nodedata(pgdat)	generic_free_nodedata(pgdat)

#ifdef CONFIG_NUMA
/*
 * If ARCH_HAS_NODEDATA_EXTENSION=n, this func is used to allocate pgdat.
 * XXX: kmalloc_node() can't work well to get new node's memory at this time.
 *	Because, pgdat for the new node is not allocated/initialized yet itself.
 *	To use new node's memory, more consideration will be necessary.
 */
#define generic_alloc_nodedata(nid)				\
({								\
	kzalloc(sizeof(pg_data_t), GFP_KERNEL);			\
})
/*
 * This definition is just for error path in node hotadd.
 * For node hotremove, we have to replace this.
 */
#define generic_free_nodedata(pgdat)	kfree(pgdat)

extern pg_data_t *node_data[];
static inline void arch_refresh_nodedata(int nid, pg_data_t *pgdat)
{
	node_data[nid] = pgdat;
}

#else /* !CONFIG_NUMA */

/* never called */
static inline pg_data_t *generic_alloc_nodedata(int nid)
{
	BUG();
	return NULL;
}
static inline void generic_free_nodedata(pg_data_t *pgdat)
{
}
static inline void arch_refresh_nodedata(int nid, pg_data_t *pgdat)
{
}
#endif /* CONFIG_NUMA */
#endif /* CONFIG_HAVE_ARCH_NODEDATA_EXTENSION */


#else /* !defined CONFIG_DYNAMIC_NODES */
static inline void pgdat_resize_lock(struct pglist_data *p, unsigned long *f) {}
static inline void pgdat_resize_unlock(struct pglist_data *p, unsigned long *f) {}
static inline void pgdat_resize_init(struct pglist_data *pgdat) {}

static inline unsigned zone_span_seqbegin(struct zone *zone)
{
	return 0;
}
static inline int zone_span_seqretry(struct zone *zone, unsigned iv)
{
	return 0;
}
static inline void zone_span_writelock(struct zone *zone) {}
static inline void zone_span_writeunlock(struct zone *zone) {}
static inline void zone_seqlock_init(struct zone *zone) {}
#endif

#endif
