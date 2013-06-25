#ifndef LINUX_MM_MEMLAYOUT_DEBUGFS_H_
#define LINUX_MM_MEMLAYOUT_DEBUGFS_H_

#include <linux/memlayout.h>

#ifdef CONFIG_DNUMA_DEBUGFS
void ml_stat_add(enum memlayout_stat stat, struct memlayout *ml, int order);
void ml_stat_inc(enum memlayout_stat stat, struct memlayout *ml);

void ml_dbgfs_memlayout_init(struct memlayout *ml);
void ml_dbgfs_memlayout_create_range(struct memlayout *ml,
				     struct rangemap_entry *rme);
void ml_dbgfs_memlayout_dini(struct memlayout *ml);

void ml_dbgfs_set_current(struct memlayout *ml);

void ml_backlog_feed(struct memlayout *ml);
#else /* !defined(CONFIG_DNUMA_DEBUGFS) */
static inline
void ml_stat_add(enum memlayout_stat stat, struct memlayout *ml, int order)
{}

static inline void ml_stat_inc(enum memlayout_stat stat, struct memlayout *ml)
{}

static inline void ml_dbgfs_memlayout_init(struct memlayout *ml)
{}
static inline void ml_dbgfs_memlayout_create_range(struct memlayout *ml,
						   struct rangemap_entry *rme)
{}
static inline void ml_dbgfs_memlayout_dini(struct memlayout *ml)
{}

static inline void ml_dbgfs_set_current(struct memlayout *ml)
{}

static inline void ml_backlog_feed(struct memlayout *ml)
{
	memlayout_destroy(ml);
}
#endif

#endif
