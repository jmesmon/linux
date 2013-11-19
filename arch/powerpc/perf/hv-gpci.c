#define pr_fmt(fmt) "hgpci: " fmt

#include <linux/perf_event.h>
#include <linux/module.h>
#include <asm/firmware.h>
#include <asm/hvcall.h>
#include <asm/hv_gpci.h>
#include <asm/io.h>

/* See arch/powerpc/include/asm/hv_gpci.h for details on the hcall interface */

PMU_RANGE_ATTR(request, config, 0, 31); /* u32 */
PMU_RANGE_ATTR(starting_index, config, 32, 63); /* u32 */
PMU_RANGE_ATTR(secondary_index, config1, 0, 15); /* u16 */
PMU_RANGE_ATTR(counter_info_version, config1, 16, 23); /* u8 */
PMU_RANGE_ATTR(length, config1, 24, 31); /* u8, size in bytes of the data (1-8) */
PMU_RANGE_ATTR(offset, config1, 32, 63); /* u32, byte offset into the returned data */

static struct attribute *format_attr[] = {
	&format_attr_request.attr,
	&format_attr_starting_index.attr,
	&format_attr_secondary_index.attr,
	&format_attr_counter_info_version.attr,

	&format_attr_offset.attr,
	&format_attr_length.attr,
	NULL,
};

static struct attribute_group format_group = {
	.name = "format",
	.attrs = format_attr,
};

static const struct attribute_group *attr_groups[] = {
	&format_group,
	NULL,
};

static unsigned long single_gpci_request(u32 req, u32 starting_index,
		u16 secondary_index, u8 version_in, u32 offset, u8 length,
		u64 *value)
{
	unsigned long ret;
	size_t i;
	u64 count;

	struct {
		struct phyp_perf_counter_info_params params;
		union {
			union h_gpci_cvs data;
			uint8_t bytes[sizeof(union h_gpci_cvs)];
		};
	} arg = {
		.params = {
			.counter_request = cpu_to_be32(req),
			.starting_index = cpu_to_be32(starting_index),
			.secondary_index = cpu_to_be16(secondary_index),
			.counter_info_version_in = version_in,
		}
	};

	ret = plpar_hcall_norets(H_GET_PERF_COUNTER_INFO, virt_to_phys(&arg), sizeof(arg));
	if (ret) {
		pr_dev("hcall failed: 0x%lx\n", ret);
		return ret;
	}

	/* TODO: verify offset and length land inside the buffer & are valid */
	count = 0;
	for (i = offset; i < offset + length; i++)
		count |= arg.bytes[i] << (i - offset);

	*value = count;
	return ret;
}

static u64 h_gpci_get_value(struct perf_event *event)
{
	u64 count;
	unsigned long ret = single_gpci_request(event_get_request(event),
						event_get_starting_index(event),
						event_get_secondary_index(event),
						event_get_counter_info_version(event),
						event_get_offset(event),
						event_get_length(event),
						&count);
	if (ret)
		return 0;
	return count;
}

static void h_gpci_event_update(struct perf_event *event)
{
	s64 prev;
	u64 now;
	now = h_gpci_get_value(event);
	prev = local64_xchg(&event->hw.prev_count, now);
	local64_add(now - prev, &event->count);
}

static void h_gpci_event_start(struct perf_event *event, int flags)
{
	local64_set(&event->hw.prev_count, h_gpci_get_value(event));
	perf_swevent_start_hrtimer(event);
}

static void h_gpci_event_stop(struct perf_event *event, int flags)
{
	perf_swevent_cancel_hrtimer(event);
	h_gpci_event_update(event);
}

static int h_gpci_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		h_gpci_event_start(event, flags);

	return 0;
}

static void h_gpci_event_del(struct perf_event *event, int flags)
{
	h_gpci_event_stop(event, flags);
}

static void h_gpci_event_read(struct perf_event *event)
{
	h_gpci_event_update(event);
}

static int h_gpci_event_init(struct perf_event *event)
{
	u64 junk;
	u8 length;

	/* config2 is unused */
	if (event->attr.config2)
		return -EINVAL;

	/* We register ourselves as a dynamic pmu, which gives us a unique type */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* unsupported modes and filters */
	if (event->attr.exclude_user   ||
	    event->attr.exclude_kernel ||
	    event->attr.exclude_hv     ||
	    event->attr.exclude_idle   ||
	    event->attr.exclude_host   ||
	    event->attr.exclude_guest  ||
	    is_sampling_event(event)) /* no sampling */
		return -EINVAL;

	/* no branch sampling */
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	length = event_get_length(event);
	if (length < 1 || lenght > 8)
		return -EINVAL;

	/* last byte within the buffer? */
	if ((event_get_offset(event) + length) > sizeof(union h_gpci_cvs))
		return -EINVAL;

	/* check if the request works... */
	if (single_gpci_request(event_get_request(event),
						event_get_starting_index(event),
						event_get_secondary_index(event),
						event_get_counter_info_version(event),
						event_get_offset(event),
						length,
						&count))
		return -EINVAL;

	perf_swevent_init_hrtimer(event);
	return 0;
}

struct pmu h_gpci_pmu = {
	.task_ctx_nr = perf_invalid_context,
	.always_schedulable = true,

	.name = "hv_gpci",
	.attr_groups = uncore_attr_groups,
	.event_init = h_gpci_event_init,
	.add = h_gpci_event_add,
	.del = h_gpci_event_del,
	.start = h_gpci_event_start,
	.stop = h_gpci_event_stop,
	.read = h_gpci_event_read,

	.event_idx = perf_swevent_event_idx,
};

static int hv_gpci_init(void)
{
	int r;

	if (!firmware_has_feature(FW_FEATURE_LPAR)) {
		pr_info("Not running under phyp, not supported\n");
		return -ENODEV;
	}

	/*
	 * XXX: We assume we always have this pmu (even if the hv doesn't
	 * actually support it) so that we don't need to reprobe on migration.
	 */

	r = perf_pmu_register(&h_gpci_pmu, h_gpci_pmu.name, -1);
	if (r)
		return r;

	return 0;
}

module_init(hv_gpci_init);
