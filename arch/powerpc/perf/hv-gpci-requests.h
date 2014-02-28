
#include "req-gen/_begin.h"

/*
 * #define REQUEST_NAME counter_request_name
 * #define REQUEST_NUM  r_num
 * #define REQUEST_IDX_KIND starting_index_kind
 * #include "_request.h"
 * REQUEST(contents)
 * #include "_request_end.h"
 * // REQUEST_(counter_request_name, counter_request_value, starting_index_kind, contents)
 *
 * - Will be used as "struct counter_request {" and "CIR_##counter_request =
 *   counter_request_value".
 * - starting_index_kind is one of:
 *   m1: must be -1
 *   chip_id: hardware chip id or -1 for current hw chip
 *   phys_processor_idx:
 *
 * __count(offset, bytes, name)
 * __field(offset, bytes, name)
 * __array(offset, bytes, name)
 *
 * #define BIT_FIELD_OFFSET offset
 * #define BIT_FIELD_BYTES  bytes
 * #define BIT_FIELD_NAME   name
 * #include "_bit_field.h"
 * __bit_field(bit_contents)
 * #include "_bit_field_end.h"
 *
 *	__count() indicates a counter that should be exposed via perf
 *	__field() indicates a normal field
 *	__array() is an array of bytes
 *	__bit_field() is a bit field, where bit_contents are:
 *		__bit(bit_offset, name)
 *
 *	@bytes for __bit_field, __count, and __field _must_ be a numeral token
 *	in decimal, not an expression.
 *
 *
 * TODO: expose secondary index, allow struct nesting
 *
 */
#define REQUEST_NAME dispatch_timebase_by_processor
#define REQUEST_NUM 0x10
#define REQUEST_IDX_KIND phys_processor_idx
#include I(REQUEST_BEGIN)
REQUEST(__count(0,	8,	processor_time_in_timebase_cycles)
	__field(0x8,	4,	hw_processor_id)
	__field(0xC,	2,	owning_part_id)
	__field(0xE,	1,	processor_state)
	__field(0xF,	1,	version)
	__field(0x10,	4,	hw_chip_id)
	__field(0x14,	4,	phys_module_id)
	__field(0x18,	4,	primary_affinity_domain_idx)
	__field(0x1C,	4,	secondary_affinity_domain_idx)
	__field(0x20,	4,	processor_version)
	__field(0x24,	2,	logical_processor_idx)
	__field(0x26,	2,	reserved)
	__field(0x28,	4,	processor_id_register)
	__field(0x2C,	4,	phys_processor_idx)
)
#include I(REQUEST_END)

#define REQUEST_NAME system_performance_capabilities
#define REQUEST_NUM 0x40
#define REQUEST_IDX_KIND m1
#include I(REQUEST_BEGIN)
REQUEST(__field(0,	1,	perf_collect_privileged)
	__field(0x1,	1  ,    capability_mask)
	__array(0x2,	0xE,	reserved)
)
#include I(REQUEST_END)

#include "req-gen/_end.h"
