#define pr_fmt(fmt) "dnuma: " fmt

#include <linux/dnuma.h>
#include <linux/spinlock.h>
#include <linux/types.h>

u64 dnuma_moved_page_ct;
DEFINE_SPINLOCK(dnuma_stats_lock);

/* Note: growth of spanned pages is not allowed if !defined(CONFIG_SPARSEMEM)
 * due to pageblock_flags */
void dnuma_adjust_spanned_pages(unsigned long pfn,
		struct zone *new_zone, struct pglist_data *new_node)
{
	unsigned long flags, end_pfn;
	pgdat_resize_lock(new_node, &flags);

	/* grow new zone */
	zone_span_writelock(new_zone);
	end_pfn = new_zone->zone_start_pfn + new_zone->spanned_pages;
	if (!new_zone->zone_start_pfn && !new_zone->spanned_pages) {
		new_zone->zone_start_pfn = pfn;
		new_zone->spanned_pages  = 1;
		pr_devel("pfn %lu: spanned zone was empty\n", pfn);
	} else if (pfn < new_zone->zone_start_pfn) {
		pr_devel("pfn %lu: zone [ %lu - %lu ) grew backwards to [ %lu - %lu )\n", pfn,
				new_zone->zone_start_pfn, end_pfn,
				pfn, end_pfn);
		new_zone->spanned_pages  = end_pfn - pfn;
		new_zone->zone_start_pfn = pfn;
	} else if (pfn >= end_pfn) {
		pr_devel("pfn %lu: zone [ %lu - %lu ) grew forwards to [ %lu - %lu )\n", pfn,
				new_zone->zone_start_pfn, end_pfn,
				new_zone->zone_start_pfn, pfn);
		new_zone->spanned_pages  = pfn - new_zone->zone_start_pfn + 1;
	}
	zone_span_writeunlock(new_zone);

	/* grow new node */
	end_pfn = new_node->node_start_pfn + new_node->node_spanned_pages;
	if (!new_node->node_start_pfn && !new_node->node_spanned_pages) {
		new_node->node_start_pfn     = pfn;
		new_node->node_spanned_pages = 1;
	} else if (pfn < new_node->node_start_pfn) {
		new_node->node_spanned_pages = end_pfn - pfn;
		new_node->node_start_pfn     = pfn;
	} else if (pfn >= end_pfn) {
		new_node->node_spanned_pages = pfn - new_node->node_start_pfn + 1;
	}

	pgdat_resize_unlock(new_node, &flags);
}
