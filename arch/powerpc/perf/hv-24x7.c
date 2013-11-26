#define pr_fmt(fmt) "24x7: " fmt

#include <linux/perf_event.h>
#include <linux/module.h>
#include <asm/firmware.h>
#include <asm/hvcall.h>
#include <asm/hv_24x7.h>
#include <asm/hv_gpci.h>
#include <asm/io.h>

/* See arch/powerpc/include/uapi/asm/24x7.h for details on the hcall
 * interface */

/* TODO: Merging events:
 * - Think of the hcall as an interface to a 3d array of counters:
 *   - x = domains
 *   - y = indexes in the domain (core, chip, vcpu, node, etc)
 *   - z = offset into the counter space
 *   - (w = lpars, but we ignore this for now. It turns this into a 4d array)
 * - A single request is: x,y,y_last,z,z_last,w,w_last
 *   - this means we can retrieve a rectangle of counters in y,z for a single x.
 *
 * - Things to consider:
 *   - input  cost_per_request = 16
 *   - output cost_per_result(ys,zs)  = 8 + 8 * ys + ys * zs
 *   - limited number of requests per hcall (must fit into 4K bytes)
 *     - 4k = 16 [buffer header] - 16 [request size] * request_count
 *     - 255 requests per hcall
 *   - sometimes it will be more efficient to read extra data and discard
 *
 * Close, but not quite. Doesn't allow for oversizing of rectangles based on cost formulation:
 * http://stackoverflow.com/questions/5919298/algorithm-for-finding-the-fewest-rectangles-to-cover-a-set-of-rectangles
 */

PMU_RANGE_ATTR(domain, config, 0, 3); /* u3 0-6, one of "PHYP_24X7_PERF_DOMAIN_*" */
/* FIXME: use a mapped attr.cpu based on meaning of domain */
PMU_RANGE_ATTR(starting_index, config, 16, 31); /* u16 */
PMU_RANGE_ATTR(offset, config, 32, 63); /* u32, see "data_offset" */
PMU_RANGE_ATTR(lpar, config1, 0, 15); /* u16 */

/* XXX: can we autogenerate these? */
PMU_RANGE_RESV(reserved1, config,   4, 15);
PMU_RANGE_RESV(reserved2, config1, 16, 63);
PMU_RANGE_RESV(reserved3, config2,  0, 63);

static struct attribute *uncore_format_attr[] = {
	&format_attr_domain.attr,
	&format_attr_offset.attr,
	&format_attr_starting_index.attr,
	&format_attr_lpar.attr,
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

struct hv_perf_caps {
	u16 version;
	u16 other_allowed:1,
	    ga:1,
	    expanded:1,
	    lab:1,
	    unused:12;
};

static unsigned long hv_perf_caps_get(struct hv_perf_caps *caps)
{
	unsigned long r;
	struct p {
		struct phyp_perf_counter_info_params params;
		struct cv_system_performance_capabilities caps;
	} __packed __aligned(sizeof(uint64_t));

	struct p arg = {
		.params = {
			.counter_request = cpu_to_be32(CIR_system_performance_capabilities),
			.starting_index = cpu_to_be32(-1),
			.counter_info_version_in = 0,
		}
	};

	r = plpar_hcall_norets(H_GET_PERF_COUNTER_INFO, virt_to_phys(&arg), sizeof(arg));
	caps->version = arg.params.counter_info_version_out;
	caps->other_allowed = arg.caps.perf_collect_privlidged;
	caps->ga = (arg.caps.capability_mask & CV_CM_GA) >> CV_CM_GA;
	caps->expanded = (arg.caps.capability_mask & CV_CM_EXPANDED) >> CV_CM_EXPANDED;
	caps->lab = (arg.caps.capability_mask & CV_CM_LAB) >> CV_CM_LAB;

	return r;
}

static unsigned long single_24x7_request(u8 domain, u32 offset, u16 ix, u16 lpar, u64 *res)
{
	unsigned long ret;

	/* XXX: this are rather large stack allocations, we probably need to
	 * grab a(some) page(s). */
	struct reqb {
		struct phyp_24x7_request_buffer buf;
		struct phyp_24x7_request req;
	} request_buffer = {
		.buf = {
			.interface_version = PHYP_24X7_IF_VERSION_CURRENT,
			.num_requests = 1,
		},
		.req = {
			.performance_domain = domain,
			.data_size = cpu_to_be16(8),
			.data_offset = cpu_to_be32(offset),
			.starting_lpar_ix = cpu_to_be16(lpar),
			.max_num_lpars = cpu_to_be16(1), /* TODO: allow more than 1 */
			.starting_ix = cpu_to_be16(ix),
			.max_ix = cpu_to_be16(1), /* TODO: allow more than 1 */
		}
	};

	struct resb {
		struct phyp_24x7_data_result_buffer buf;
		struct phyp_24x7_result res;
		struct phyp_24x7_result_element elem;
		__be64 result;
	} result_buffer = {};

	ret = plpar_hcall_norets(H_GET_24X7_DATA,
			virt_to_phys(&request_buffer), sizeof(request_buffer),
			virt_to_phys(&result_buffer),  sizeof(result_buffer));

	if (!ret) {
		/* something failed, we don't have data, there may be some
		 * extra data on the failure */
		pr_err_ratelimited("hcall failed: 0x%lx (%ld) detail=0x%x"
				" failing ix=%x\n", ret, ret,
				result_buffer.buf.detailed_rc,
				result_buffer.buf.failing_request_ix);
		return ret;
	}

	*res = be64_to_cpu(result_buffer.result);
	return ret;
}

static int h_24x7_event_init(struct perf_event *event)
{
	struct phyp_perf_caps caps;
	unsigned domain;
	u64 ct;

	/* Unused areas must be 0 */
	if (event_get_reserved1(event) ||
	    event_get_reserved2(event) ||
	    event_get_reserved3(event))
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

	/*
	 * TODO: event->cpu is exactly the 'cpu' argument to perf_event_open(),
	 * we need to interprate it (and adjust to common cpu) based on whether
	 * we are retrieving a per-core, per-vcpu, per-cpu, per-chip, per-lpar,
	 * or system wide event.
	 */

	/* no branch sampling */
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	/* offset must be 8 byte aligned */
	if (event_get_offset(event) % 8)
		return -EINVAL;

	/* Domains above 6 are invalid */
	domain = event_get_domain(event);
	if (domain > 6)
		return -EINVAL;

	if (get_phyp_perf_caps(&caps))
		return -EIO;

	/* PHYSICAL domains & other lpars require extra capabilities */
	if (!caps.other_allowed && ((domain == PHYP_24X7_PERF_DOMAIN_PHYSICAL_CHIP ||
		domain == PHYP_24X7_PERF_DOMAIN_PHYSICAL_CORE) ||
		(event_get_lpar(event) != event_get_lpar_max())))
		return -EPERM;

	/* see if the event complains */
	if (single_24x7_request(event_get_domain(event),
				event_get_offset(event),
				event_get_starting_index(event),
				event_get_lpar(event),
				&ct))
		return -EIO;

	perf_swevent_init_hrtimer(event);
	return 0;
}

static u64 h_24x7_get_value(struct perf_event *event)
{
	unsigned long ret;
	u64 ct;

	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL, DEFAULT_RATELIMIT_BURST);
	bool pr = false;
	if (__ratelimit(&rs))
		pr = true;

	ret = single_24x7_request(event_get_domain(event),
				  event_get_offset(event),
				  event_get_starting_index(event),
				  event_get_lpar(event),
				  &ct);
	if (ret)
		/* We checked this in event init, shouldn't fail here... */
		return 0;

	return ct;
}

static void h_24x7_event_update(struct perf_event *event)
{
	s64 prev;
	u64 now;
	now = h_24x7_get_value(event);
	prev = local64_xchg(&event->hw.prev_count, now);
	local64_add(now - prev, &event->count);
}

static void h_24x7_event_start(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_RELOAD)
		local64_set(&event->hw.prev_count, h_24x7_get_value(event));
	perf_swevent_start_hrtimer(event);
}

static void h_24x7_event_stop(struct perf_event *event, int flags)
{
	perf_swevent_cancel_hrtimer(event);
	h_24x7_event_update(event);
}

static int h_24x7_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		h_24x7_event_start(event, flags);

	return 0;
}

static void h_24x7_event_del(struct perf_event *event, int flags)
{
	h_24x7_event_stop(event, flags);
}

static void h_24x7_event_read(struct perf_event *event)
{
	h_24x7_event_update(event);
}

struct pmu h_24x7_pmu = {
	/*
	 * Tell core to allocate a pmu-local set of per-cpu pmu_cpu_contexts.
	 * We really don't have any need for a "context" (it would only really
	 * make sense to have a ctx per-event-group), but this works (and lets
	 * us avoid sharing the context with other pmus).
	 */
	.task_ctx_nr = perf_invalid_context,
	.always_schedulable = true,

	.name = "hv_24x7",
	.attr_groups = uncore_attr_groups,
	.event_init  = h_24x7_event_init,
	.add         = h_24x7_event_add,
	.del         = h_24x7_event_del,
	.start       = h_24x7_event_start,
	.stop        = h_24x7_event_stop,
	.read        = h_24x7_event_read,

	.event_idx = perf_swevent_event_idx,
};

static int hv_24x7_init(void)
{
	int r;
	struct phyp_perf_caps caps;

	BUILD_BUG_ON(sizeof(struct phyp_24x7_request_buffer) != 0x10);

	if (!firmware_has_feature(FW_FEATURE_LPAR)) {
		pr_info("not an lpar, disabled\n");
		return -ENODEV;
	}

	if (!hv_perf_caps_get(&caps)) {
		pr_info("could not obtain capabilities, disabled\n");
		return -ENODEV;
	}

	pr_info("gpci interface versions: hv:0x%x, kernel:0x%x \n",
			caps.version, COUNTER_INFO_VERSION_CURRENT);

	r = perf_pmu_register(&h_24x7_pmu, h_24x7_pmu.name, -1);
	if (r)
		return r;

	return 0;
}

module_init(hv_24x7_init);
