#include <linux/spinlock.h>
#include <linux/types.h>

u64 dnuma_moved_page_ct;
DEFINE_SPINLOCK(dnuma_stats_lock);
