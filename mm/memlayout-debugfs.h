#ifndef LINUX_MM_MEMLAYOUT_DEBUGFS_H_
#define LINUX_MM_MEMLAYOUT_DEBUGFS_H_

#include <linux/memlayout.h>

#ifdef CONFIG_DNUMA_DEBUGFS
void ml_stat_count_moved_pages(int order);
void ml_stat_cache_hit(void);
void ml_stat_cache_miss(void);
void ml_dbgfs_init(struct memlayout *ml);
void ml_dbgfs_create_range(struct memlayout *ml, struct rangemap_entry *rme);
void ml_destroy_dbgfs(struct memlayout *ml);
void ml_dbgfs_set_current(struct memlayout *ml);
void ml_backlog_feed(struct memlayout *ml);
#else /* !defined(CONFIG_DNUMA_DEBUGFS) */
static inline void ml_stat_count_moved_pages(int order)
{}
static inline void ml_stat_cache_hit(void)
{}
static inline void ml_stat_cache_miss(void)
{}

static inline void ml_dbgfs_init(struct memlayout *ml)
{}
static inline void ml_dbgfs_create_range(struct memlayout *ml,
		struct rangemap_entry *rme)
{}
static inline void ml_destroy_dbgfs(struct memlayout *ml)
{}
static inline void ml_dbgfs_set_current(struct memlayout *ml)
{}

static inline void ml_backlog_feed(struct memlayout *ml)
{
	memlayout_destroy(ml);
}
#endif

#endif
