#undef TRACE_SYSTEM
#define TRACE_SYSTEM memlayout

#if !defined(_TRACE_MEMLAYOUT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MEMLAYOUT_H

#include <linux/memlayout.h>
#include <linux/tracepoint.h>

TRACE_EVENT(memlayout_cache_access,
	TP_PROTO(struct memlayout *ml, bool hit),
	TP_ARGS(ml, hit),
	TP_STRUCT__entry(
		__field(int,  num)
		__field(bool, hit)
	),
	TP_fast_assign(
		__entry->num = ml->seq;
		__entry->hit = hit;
	),
	TP_printk("memlayout %d cache %s",
		__entry->num,
		__entry->hit ? "hit" : "miss")
	);

#endif

#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE memlayout-trace

#include <trace/define_trace.h>
