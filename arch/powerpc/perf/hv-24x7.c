/*
 * Hypervisor supplied "24x7" performance counter support
 *
 * Author: Cody P Schafer <cody@linux.vnet.ibm.com>
 * Copyright 2014 IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt) "hv-24x7: " fmt

#include <linux/perf_event.h>
#include <linux/module.h>
#include <asm/firmware.h>
#include <asm/hvcall.h>
#include <asm/hv_24x7.h>
#include <asm/hv_gpci.h>
#include <asm/io.h>

/* See arch/powerpc/include/asm/24x7.h for details on the hcall interface */

/* TODO: Merging events:
 * - Think of the hcall as an interface to a 4d array of counters:
 *   - x = domains
 *   - y = indexes in the domain (core, chip, vcpu, node, etc)
 *   - z = offset into the counter space
 *   - w = lpars (guest vms, "logical partitions")
 * - A single request is: x,y,y_last,z,z_last,w,w_last
 *   - this means we can retrieve a rectangle of counters in y,z for a single x.
 *
 * - Things to consider (ignoring w):
 *   - input  cost_per_request = 16
 *   - output cost_per_result(ys,zs)  = 8 + 8 * ys + ys * zs
 *   - limited number of requests per hcall (must fit into 4K bytes)
 *     - 4k = 16 [buffer header] - 16 [request size] * request_count
 *     - 255 requests per hcall
 *   - sometimes it will be more efficient to read extra data and discard
 */

PMU_RANGE_ATTR(domain, config, 0, 3); /* u3 0-6, one of HV_24X7_PERF_DOMAIN */
PMU_RANGE_ATTR(starting_index, config, 16, 31); /* u16 */
PMU_RANGE_ATTR(offset, config, 32, 63); /* u32, see "data_offset" */
PMU_RANGE_ATTR(lpar, config1, 0, 15); /* u16 */

PMU_RANGE_RESV(reserved1, config,   4, 15);
PMU_RANGE_RESV(reserved2, config1, 16, 63);
PMU_RANGE_RESV(reserved3, config2,  0, 63);

static struct attribute *format_attr[] = {
	&format_attr_domain.attr,
	&format_attr_offset.attr,
	&format_attr_starting_index.attr,
	&format_attr_lpar.attr,
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
		struct hv_get_perf_counter_info_params params;
		struct cv_system_performance_capabilities caps;
	} __packed __aligned(sizeof(uint64_t));

	struct p arg = {
		.params = {
			.counter_request = cpu_to_be32(
					CIR_system_performance_capabilities),
			.starting_index = cpu_to_be32(-1),
			.counter_info_version_in = 0,
		}
	};

	r = plpar_hcall_norets(H_GET_PERF_COUNTER_INFO,
			       virt_to_phys(&arg), sizeof(arg));
	caps->version = arg.params.counter_info_version_out;
	caps->other_allowed = arg.caps.perf_collect_privlidged;
	caps->ga = (arg.caps.capability_mask & CV_CM_GA) >> CV_CM_GA;
	caps->expanded = (arg.caps.capability_mask & CV_CM_EXPANDED)
				>> CV_CM_EXPANDED;
	caps->lab = (arg.caps.capability_mask & CV_CM_LAB) >> CV_CM_LAB;

	return r;
}

static bool is_physical_domain(int domain)
{
	return  domain == HV_24X7_PERF_DOMAIN_PHYSICAL_CHIP ||
		domain == HV_24X7_PERF_DOMAIN_PHYSICAL_CORE;
}

static unsigned long single_24x7_request(u8 domain, u32 offset, u16 ix,
					 u16 lpar, u64 *res)
{
	unsigned long ret;
	struct reqb {
		struct hv_24x7_request_buffer buf;
		struct hv_24x7_request req;
	} request_buffer = {
		.buf = {
			.interface_version = HV_24X7_IF_VERSION_CURRENT,
			.num_requests = 1,
		},
		.req = {
			.performance_domain = domain,
			.data_size = cpu_to_be16(8),
			.data_offset = cpu_to_be32(offset),
			.starting_lpar_ix = cpu_to_be16(lpar),
			.max_num_lpars = cpu_to_be16(1),
			.starting_ix = cpu_to_be16(ix),
			.max_ix = cpu_to_be16(1),
		}
	};

	struct resb {
		struct hv_24x7_data_result_buffer buf;
		struct hv_24x7_result res;
		struct hv_24x7_result_element elem;
		__be64 result;
	} result_buffer = {};

	ret = plpar_hcall_norets(H_GET_24X7_DATA,
			virt_to_phys(&request_buffer), sizeof(request_buffer),
			virt_to_phys(&result_buffer),  sizeof(result_buffer));

	if (ret) {
		/*
		 * this failure is unexpected since we check if the exact same
		 * hcall works in event_init
		 */
		pr_err_ratelimited("hcall failed: %d %#x %#x %d => 0x%lx (%ld) detail=0x%x failing ix=%x\n",
				domain, offset, ix, lpar,
				ret, ret,
				result_buffer.buf.detailed_rc,
				result_buffer.buf.failing_request_ix);
		return ret;
	}

	*res = be64_to_cpu(result_buffer.result);
	return ret;
}

static int h_24x7_event_init(struct perf_event *event)
{
	struct hv_perf_caps caps;
	unsigned domain;
	u64 ct;

	/* Not our event */
	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	/* Unused areas must be 0 */
	if (event_get_reserved1(event) ||
	    event_get_reserved2(event) ||
	    event_get_reserved3(event)) {
		pr_devel("reserved set when forbidden 0x%llx(0x%llx) 0x%llx(0x%llx) 0x%llx(0x%llx)\n",
				event->attr.config,
				event_get_reserved1(event),
				event->attr.config1,
				event_get_reserved2(event),
				event->attr.config2,
				event_get_reserved3(event));
		return -EINVAL;
	}

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

	/* offset must be 8 byte aligned */
	if (event_get_offset(event) % 8) {
		pr_devel("bad alignment\n");
		return -EINVAL;
	}

	/* Domains above 6 are invalid */
	domain = event_get_domain(event);
	if (domain > 6) {
		pr_devel("invalid domain\n");
		return -EINVAL;
	}

	if (hv_perf_caps_get(&caps)) {
		pr_devel("could not get capabilities\n");
		return -EIO;
	}

	/* PHYSICAL domains & other lpars require extra capabilities */
	if (!caps.other_allowed && (is_physical_domain(domain) ||
		(event_get_lpar(event) != event_get_lpar_max()))) {
		pr_devel("hv permisions disallow\n");
		return -EPERM;
	}

	/* see if the event complains */
	if (single_24x7_request(event_get_domain(event),
				event_get_offset(event),
				event_get_starting_index(event),
				event_get_lpar(event),
				&ct)) {
		pr_devel("test hcall failed\n");
		return -EIO;
	}

	/*
	 * Some of the events are per-cpu, some per-core, some per-chip, some
	 * are global, and some access data from other virtual machines on the
	 * same physical machine. We can't map the cpu value without a lot of
	 * work. Instead, we pick an arbitrary cpu for all events on this pmu.
	 */
	event->cpu = 0;

	perf_swevent_init_hrtimer(event);
	return 0;
}

static u64 h_24x7_get_value(struct perf_event *event)
{
	unsigned long ret;
	u64 ct;
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
	.task_ctx_nr = perf_invalid_context,

	.name = "hv_24x7",
	.attr_groups = attr_groups,
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
	unsigned long hret;
	struct hv_perf_caps caps;

	if (!firmware_has_feature(FW_FEATURE_LPAR)) {
		pr_info("not an lpar, disabled\n");
		return -ENODEV;
	}

	hret = hv_perf_caps_get(&caps);
	if (hret) {
		pr_info("could not obtain capabilities, error 0x%80lx, disabling\n",
				hret);
		return -ENODEV;
	}

	pr_info("gpci interface versions: hv:0x%x, kernel:0x%x\n",
			caps.version, COUNTER_INFO_VERSION_CURRENT);

	pr_info("gpci interface capabilities: other:%d ga:%d expanded:%d lab:%d\n",
			caps.other_allowed, caps.ga,
			caps.expanded,
			caps.lab);

	r = perf_pmu_register(&h_24x7_pmu, h_24x7_pmu.name, -1);
	if (r)
		return r;

	return 0;
}

module_init(hv_24x7_init);
