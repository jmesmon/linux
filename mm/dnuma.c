#define pr_fmt(fmt) "dnuma: " fmt

#include <linux/dnuma.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#if CONFIG_DNUMA_DEBUGFS
u64 dnuma_moved_page_ct;
DEFINE_SPINLOCK(dnuma_stats_lock);
#endif

/* Options wrt removing free pages from free lists:
 * - Isolation? - NO.
 *   - Can't determine which page ranges to isolate (memory belonging to this
 *     zone could be anywhwre)
 *   - too many things being isolated could trigger OOM.
 *   - Not too terrible if some stuff is allocated.
 * - lock zone & then grab a "bunch", lock new zone & give it a "bunch".
 * - Selecting pages to move
 *   - iterate over pfns
 *     - with dnuma, zones grow very large, could result in iterating over all
 *       pfns on the system.
 *     - pfns we examine could belong to a different zone, requiring skipping.
 *     - Fits with "destination" focus
 *   - iterate over free lists
 *     - pointer dereferences are not as fast as addition (pfn ++, etc).
 *     - all pages belong to the current zone, no skipping.
 *     - Fits with "source" focus
 * - Focus on the source zone or the destination zone (complexities are per
 *   zone operation, so don't really represent actual cost)
 *   - source
 *     - grab all pages in 1 source zone that need moving to any number of
 *       other zones.
 *     - repeat for each source zone. TC=O(n), MC=O(n).
 *   - destination
 *     - grab all pages from various sources which will be moved to 1
 *       destination.
 *     - repeat for each destination zone. TC=O(n), MC=O(1).
 *     - Potentially very high amount of locking/unlocking.
 *   - mixed
 *     - grab all pages in 1 source which need moving to 1 other zone.
 *     - repeat for all pairs of zones. TC = O(n^n), MC = O(1).
 *     - High time complexity. Does this actually represent a real slowdown?
 */

/* Issues due to pageflag_blocks attached to zones with Discontig Mem (&
 * Flatmem??).
 * - Need atomicity over the combination of commiting a new memlayout and
 *   removing the pages from free lists.
 */

/* - must be called under lock_memory_hotplug() */
void dnuma_online_required_nodes(struct memlayout *new_ml)
{
	int nid;
	struct rangemap_entry *rme;
	ml_for_each_range(new_ml, rme) {
		nid = rme->nid;
		if (!node_online(nid))
			mem_online_node(nid);
	}
}

void dnuma_move_to_new_ml(struct memlayout *new_ml)
{
}
