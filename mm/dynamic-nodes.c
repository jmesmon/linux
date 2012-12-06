#include <linux/dynamic-nodes.h>

static void grow_zone_span(struct zone *zone, unsigned long start_pfn,
			   unsigned long end_pfn)
{
	unsigned long old_zone_end_pfn;

	zone_span_writelock(zone);

	old_zone_end_pfn = zone->zone_start_pfn + zone->spanned_pages;
	if (start_pfn < zone->zone_start_pfn)
		zone->zone_start_pfn = start_pfn;

	zone->spanned_pages = max(old_zone_end_pfn, end_pfn) -
				zone->zone_start_pfn;

	zone_span_writeunlock(zone);
}

static void grow_pgdat_span(struct pglist_data *pgdat, unsigned long start_pfn,
			    unsigned long end_pfn)
{
	unsigned long old_pgdat_end_pfn =
		pgdat->node_start_pfn + pgdat->node_spanned_pages;

	if (start_pfn < pgdat->node_start_pfn)
		pgdat->node_start_pfn = start_pfn;

	pgdat->node_spanned_pages = max(old_pgdat_end_pfn, end_pfn) -
					pgdat->node_start_pfn;
}

void grow_pgdat_and_zone(struct zone *zone, unsigned long start_pfn,
			 unsigned long end_pfn)
{
	unsigned long flags;
	pgdat_resize_lock(zone->zone_pgdat, &flags);
	grow_zone_span(zone, start_pfn, end_pfn);
	grow_pgdat_span(zone->zone_pgdat, start_pfn, end_pfn);
	pgdat_resize_unlock(zone->zone_pgdat, &flags);
}

void adjust_zone_present_pages(struct zone *zone, long delta)
{
	pgdat_resize_lock(zone->zone_pgdat, &flags);
	zone_span_writelock(zone);

	zone->present_pages += delta;
	zone->zone_pgdat->node_present_pages += delta;

	zone_span_writeunlock(zone);
	pgdat_resize_unlock(zone->zone_pgdat, &flags);
}
