#define pr_fmt(fmt) "hgpci: " fmt

#include <linux/perf_event.h>
#include <linux/module.h>
#include <asm/firmware.h>
#include <asm/hvcall.h>
#include <asm/h_counter_info.h>
#include <asm/io.h>

/* See arch/powerpc/include/uapi/asm/h_counter_info.h for details on the hcall
 * interface */

PMU_RANGE_ATTR(request, config, 0, 31); /* u32 */
PMU_RANGE_ATTR(starting_index, config, 32, 63); /* u32 */
PMU_RANGE_ATTR(secondary_index, config1, 0, 15); /* u16 */
PMU_RANGE_ATTR(counter_info_version, config1, 16, 23); /* u8 */
PMU_RANGE_ATTR(offset, config2, 0, 31); /* u32, byte offset into the returned data */
PMU_RANGE_ATTR(length, config1, 24, 32); /* u8, size in bytes of the data (1-8) */

static struct attribute *uncore_format_attr[] = {
	&format_attr_request.attr,
	&format_attr_starting_index.attr,
	&format_attr_secondary_index.attr,
	&format_attr_counter_info_version.attr,

	&format_attr_offset.attr,
	&format_attr_length.attr,
	NULL,
};

static struct attribute_group uncore_format_group = {
	.name = "format",
	.attrs = uncore_format_attr,
};

static const struct attribute_group *uncore_attr_groups[] = {
	//&uncore_event_group,
	&uncore_format_group,
	NULL,
};

static int h_gpci_event_init(struct perf_event *event)
{
	//struct hw_perf_event *hwc = &event->hw;

	/* We register ourselves as a dynamic pmu, which gives us a unique type */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

#if 0
	/* We can't do per-task sampling */
	if (is_sampling_event(event) || event->attach_state & PERF_ATTACH_TASK)
		return -EINVAL;

	/* We can't exclude anything */
	if (event->attr.exclude_user || event->attr.exclude_kernel ||
	    event->attr.exclude_hv   || event->attr.exclude_idle   ||
	    event->attr.exclude_host || event->attr.exclude_guest)
		return -EINVAL;

	/* We don't get "events", polling is required */
	if (!hwc->sample_period)
		return -EINVAL;

	/* nope */
	if (event->attr.sample_type & PERF_SAMPLE_IP ||
	    event->attr.sample_type & PERF_SAMPLE_CALLCHAIN)
		return -EINVAL;

	/* We can't actually stop these counters */
	hwc->state = 0;

	/* XXX: what about all the other ev->hw fields? What is using them? */

	/* TODO: event->cpu is exactly the 'cpu' argument to perf_event_open(),
	 * we need to interprate it based on whether we are retrieving a
	 * per-core, per-vcpu, per-cpu, per-chip, per-lpar, or system wide
	 * event */

	/* TODO: Based on intel & amd's uncore events, we need to adjust
	 * event->cpu to a common cpu for per-core, per-chip, and system wide
	 * events */
#endif

	/*
	 * no branch sampling for software events
	 */
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	perf_swevent_init_hrtimer(event);
	return 0;
}

static u64 h_gpci_get_value(struct perf_event *event)
{
	unsigned long ret;
	size_t offset;
	int bytes;
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
			.counter_request = cpu_to_be32(event_get_request(event)),
			.starting_index = cpu_to_be32(event_get_starting_index(event)),
			.secondary_index = cpu_to_be16(event_get_secondary_index(event)),
			.counter_info_version_in = event_get_counter_info_version(event),
		}
	};

	ret = plpar_hcall_norets(H_GET_PERF_COUNTER_INFO, virt_to_phys(&arg), sizeof(arg));
	if (ret) {
		/* TODO: check this before allowing the event to be setup */
		pr_err("hcall failed: 0x%lx\n", ret);
		return 0;
	}

	/* XXX: verify offset and length land inside the buffer & are valid */
	offset = event_get_offset(event);
	bytes = event_get_length(event);
	count = 0;
	for (i = offset; i < offset + bytes; i++)
		count |= arg.bytes[i] << (i - offset);

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

struct pmu h_gpci_pmu = {
	.name = "phyp_hgpci",
	.attr_groups = uncore_attr_groups,
	.event_init = h_gpci_event_init,
	.add = h_gpci_event_add,
	.del = h_gpci_event_del,
	.start = h_gpci_event_start,
	.stop = h_gpci_event_stop,
	.read = h_gpci_event_read,

	.event_idx = perf_swevent_event_idx,
};

static int phyp_uncore_init(void)
{
	int r;

	if (!firmware_has_feature(FW_FEATURE_LPAR)) {
		pr_info("Not running under phyp, not supported\n");
		return -ENODEV;
	}

	/* TODO: detect versioning here? Or later? How will we handle migration
	 * between VMs with different supported versions? Should the PMU go
	 * away and then come back? (Probably) */

	r = perf_pmu_register(&h_gpci_pmu, h_gpci_pmu.name, -1);
	if (r)
		return r;

	return 0;
}

module_init(phyp_uncore_init);
