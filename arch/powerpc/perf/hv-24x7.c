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

#include <linux/byteorder.h>
#include <linux/perf_event.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <asm/firmware.h>
#include <asm/hvcall.h>
#include <asm/io.h>

#include "hv-24x7.h"
#include "hv-24x7-catalog.h"
#include "hv-common.h"

static const char *domain_to_index_string(unsigned domain)
{
	switch (domain) {
#define DOMAIN(n, p, v, x)		\
	case HV_PERF_DOMAIN_##n:	\
		return #x;
#include "hv-24x7-domains.h"
#undef DOMAIN
	default:
		WARN(1, "unknown domain %d\n", domain);
		BUG();
	}
}

static const char *_event_domain_suffix(unsigned domain)
{
	switch (domain) {
#define DOMAIN(n, p, _v, x)		\
	case HV_PERF_DOMAIN_##n:		\
		return #p;
#include "hv-24x7-domains.h"
#undef DOMAIN
	default:
		WARN(1, "unknown domain %d\n", domain);
		BUG();
	}
}

static bool domain_is_valid(unsigned domain)
{
	switch (domain) {
#define DOMAIN(n, p, v, x)		\
	case HV_PERF_DOMAIN_##n:	\
		/* fall through */
#include "hv-24x7-domains.h"
#undef DOMAIN
		return true;
	default:
		return false;
	}
}

static const char *event_domain_suffix(unsigned domain)
{
	if (domain == HV_PERF_DOMAIN_PHYSICAL_CORE)
		return NULL;
	else
		return _event_domain_suffix(domain);
}

/*
 * TODO: Merging events:
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

PMU_FORMAT_RANGE(domain, config, 0, 3); /* u3 0-6, one of HV_PERF_DOMAIN */
PMU_FORMAT_RANGE(starting_index, config, 16, 31); /* u16 */
PMU_FORMAT_RANGE(offset, config, 32, 63); /* u32, see "data_offset" */
PMU_FORMAT_RANGE(lpar, config1, 0, 15); /* u16 */

EVENT_DEFINE_RANGE(reserved1, config,   4, 15);
EVENT_DEFINE_RANGE(reserved2, config1, 16, 63);
EVENT_DEFINE_RANGE(reserved3, config2,  0, 63);

static struct attribute *format_attrs[] = {
	&format_attr_domain.attr,
	&format_attr_offset.attr,
	&format_attr_starting_index.attr,
	&format_attr_lpar.attr,
	NULL,
};

static struct attribute_group format_group = {
	.name = "format",
	.attrs = format_attrs,
};

static struct attribute_group event_group = {
	.name = "events",
	/* .attrs is set in init */
};

static struct kmem_cache *hv_page_cache;

/*
 * read_offset_data - copy data from one buffer to another while treating the
 *                    source buffer as a small view on the total avaliable
 *                    source data.
 *
 * @dest: buffer to copy into
 * @dest_len: length of @dest in bytes
 * @requested_offset: the offset within the source data we want. Must be > 0
 * @src: buffer to copy data from
 * @src_len: length of @src in bytes
 * @source_offset: the offset in the sorce data that (src,src_len) refers to.
 *                 Must be > 0
 *
 * returns the number of bytes copied.
 *
 * The following ascii art shows the various buffer possitioning we need to
 * handle, assigns some arbitrary varibles to points on the buffer, and then
 * shows how we fiddle with those values to get things we care about (copy
 * start in src and copy len)
 *
 * s = @src buffer
 * d = @dest buffer
 * '.' areas in d are written to.
 *
 *                       u
 *   x         w	 v  z
 * d           |.........|
 * s |----------------------|
 *
 *                      u
 *   x         w	z     v
 * d           |........------|
 * s |------------------|
 *
 *   x         w        u,z,v
 * d           |........|
 * s |------------------|
 *
 *   x,w                u,v,z
 * d |..................|
 * s |------------------|
 *
 *   x        u
 *   w        v		z
 * d |........|
 * s |------------------|
 *
 *   x      z   w      v
 * d            |------|
 * s |------|
 *
 * x = source_offset
 * w = requested_offset
 * z = source_offset + src_len
 * v = requested_offset + dest_len
 *
 * w_offset_in_s = w - x = requested_offset - source_offset
 * z_offset_in_s = z - x = src_len
 * v_offset_in_s = v - x = request_offset + dest_len - src_len
 */
static ssize_t read_offset_data(void *dest, size_t dest_len,
				loff_t requested_offset, void *src,
				size_t src_len, loff_t source_offset)
{
	size_t w_offset_in_s = requested_offset - source_offset;
	size_t z_offset_in_s = src_len;
	size_t v_offset_in_s = requested_offset + dest_len - src_len;
	size_t u_offset_in_s = min(z_offset_in_s, v_offset_in_s);
	size_t copy_len = u_offset_in_s - w_offset_in_s;

	if (requested_offset < 0 || source_offset < 0)
		return -EINVAL;

	if (z_offset_in_s <= w_offset_in_s)
		return 0;

	memcpy(dest, src + w_offset_in_s, copy_len);
	return copy_len;
}

static char *event_name(struct hv_24x7_event_data *ev, size_t *len)
{
	*len = be_to_cpu(ev->event_name_len) - 2;
	return (char *)ev->remainder;
}

static bool event_fixed_portion_is_within(struct hv_24x7_event_data *ev, void *end)
{
	void *start = ev;
	return (start + offsetof(struct hv_24x7_event_data, remainder)) < end;
}

/*
 * Things we don't check:
 *  - padding for desc, name, and long/detailed desc is required to be '\0' bytes.
 *
 *  Return NULL if we pass end,
 *  Otherwise return the address of the byte just following the event.
 */
static void *event_end(struct hv_24x7_event_data *ev, void *end)
{
	void *start = ev;
	__be16 *dl_, *ldl_;
	unsigned dl, ldl;
	unsigned nl = be_to_cpu(ev->event_name_len);

	if (nl < 2) {
		pr_debug("%s: name length too short: %d", __func__, nl);
		return NULL;
	}

	if (start + nl > end) {
		pr_debug("%s: start=%p + nl=%u > end=%p", __func__, start, nl, end);
		return NULL;
	}

	dl_ = (__be16 *)(ev->remainder + nl - 2);
	if (!IS_ALIGNED((uintptr_t)dl_, 2))
		pr_warn("desc len not aligned %p", dl_);
	dl = be_to_cpu(*dl_);
	if (dl < 2) {
		pr_debug("%s: desc len too short: %d", __func__, dl);
		return NULL;
	}

	if (start + nl + dl > end) {
		pr_debug("%s: (start=%p + nl=%u + dl=%u)=%p > end=%p", __func__, start, nl, dl, start + nl + dl, end);
		return NULL;
	}

	ldl_ = (__be16 *)(ev->remainder + nl + dl - 2);
	if (!IS_ALIGNED((uintptr_t)ldl_, 2))
		pr_warn("long desc len not aligned %p", ldl_);
	ldl = be_to_cpu(*ldl_);
	if (ldl < 2) {
		pr_debug("%s: long desc len too short (ldl=%u)", __func__, ldl);
		return NULL;
	}

	if (start + nl + dl + ldl > end) {
		pr_debug("%s: start=%p + nl=%u + dl=%u + ldl=%u > end=%p", __func__, start, nl, dl, ldl, end);
		return NULL;
	}

	return start + nl + dl + ldl;
}

static unsigned long h_get_24x7_catalog_page_(unsigned long phys_4096,
					      u32 version, u32 index)
{
	pr_devel("h_get_24x7_catalog_page(0x%lx, %lu, %lu)",
			phys_4096,
			(unsigned long)version,
			(unsigned long)index);
	WARN_ON(!IS_ALIGNED(phys_4096, 4096));
	return plpar_hcall_norets(H_GET_24X7_CATALOG_PAGE,
			phys_4096,
			version,
			index);
}

static unsigned long h_get_24x7_catalog_page(char page[],
					     u64 version, u32 index)
{
	return h_get_24x7_catalog_page_(virt_to_phys(page),
					version, index);
}

unsigned core_domains [] = {
	HV_PERF_DOMAIN_PHYSICAL_CORE,
	HV_PERF_DOMAIN_VIRTUAL_PROCESSOR_HOME_CORE,
	HV_PERF_DOMAIN_VIRTUAL_PROCESSOR_HOME_CHIP,
	HV_PERF_DOMAIN_VIRTUAL_PROCESSOR_HOME_NODE,
	HV_PERF_DOMAIN_VIRTUAL_PROCESSOR_REMOTE_NODE,
};
/* chip event data always yeilds a single event, core yeilds multiple */
#define MAX_EVENTS_PER_EVENT_DATA ARRAY_SIZE(core_domains)

static char *event_fmt(struct hv_24x7_event_data *event, unsigned domain)
{
	return kasprintf(GFP_KERNEL,
			"domain=0x%x,offset=0x%x,starting_index=%s,lpar=sibling_guest_id",
			domain,
			be_to_cpu(event->event_counter_offs) + be_to_cpu(event->event_group_record_offs),
			domain_to_index_string(event->domain));
}

static struct attribute *event_to_attr(unsigned ix, struct hv_24x7_event_data *event, unsigned domain)
{
	size_t event_name_len, suffix_space;
	char *ev_name, *a_ev_name;
	const char *ev_suffix;
	struct perf_pmu_events_attr *attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return NULL;

	if (!domain_is_valid(domain)) {
		pr_warn("catalog event %u has invalid domain %u\n", ix, domain);
		return NULL;
	}

	attr->event_str = event_fmt(event, domain);
	if (!attr->event_str)
		goto e_attr;

	ev_suffix = event_domain_suffix(domain);
	suffix_space = ev_suffix ? 2 + strlen(ev_suffix) : 0;
	ev_name = event_name(event, &event_name_len);
	if (ev_suffix)
		a_ev_name = kasprintf(GFP_KERNEL, "%.*s__%s", (int)event_name_len, ev_name, ev_suffix);
	else
		a_ev_name = kasprintf(GFP_KERNEL, "%.*s", (int)event_name_len, ev_name);

	if (!a_ev_name)
		goto e_event_fmt;

	sysfs_attr_init(&attr->attr);
	attr->attr.attr.name = a_ev_name;
	attr->attr.attr.mode = 0444;
	attr->attr.show = perf_event_sysfs_show;
	return &attr->attr.attr;

e_event_fmt:
	kfree(attr->event_str);
e_attr:
	kfree(attr);
	return NULL;
}

static void event_attr_destroy(struct attribute *attr)
{
	struct perf_pmu_events_attr *ev_attr = container_of(attr, struct perf_pmu_events_attr, attr.attr);
	kfree(attr->name);
	kfree(ev_attr->event_str);
	kfree(ev_attr);
}

static ssize_t event_data_to_attrs(unsigned ix, struct attribute **attrs,
		struct hv_24x7_event_data *event)
{
	unsigned i;
	switch (event->domain) {
	case HV_PERF_DOMAIN_PHYSICAL_CHIP:
		*attrs = event_to_attr(ix, event, event->domain);
		return 1;
	case HV_PERF_DOMAIN_PHYSICAL_CORE:
		for (i = 0; i < ARRAY_SIZE(core_domains); i++) {
			attrs[i] = event_to_attr(ix, event, core_domains[i]);
			if (!attrs[i]) {
				pr_warn("catalog event %u: individual attr %u creation failure\n",
						ix, i);
				for (; i ; i--)
					event_attr_destroy(attrs[i - 1]);
				return -1;
			}
		}
		return i;
	default:
		pr_warn("catalog event %u: domain %u is not allowed in the catalog\n",
				ix, event->domain);
		return -1;
	}
}

static unsigned long vmalloc_to_phys(void *v)
{
	struct page *p = vmalloc_to_page(v);
	BUG_ON(!p);
	return page_to_phys(p) + offset_in_page(v);
}

static struct attribute **create_events_from_catalog(void)
{
	unsigned long hret;
	size_t catalog_len, catalog_page_len, event_entry_count,
	       event_data_len, event_data_offs,
	       event_data_bytes, junk_events, event_idx, event_ct, i,
	       attr_max;
	ssize_t ct;
	uint32_t catalog_version_num;
	struct attribute **events;
	struct hv_24x7_catalog_page_0 *page_0 = kmem_cache_alloc(hv_page_cache, GFP_KERNEL);
	void *page = page_0;
	void *event_data, *end;
	struct hv_24x7_event_data *event;

	if (!page)
		return NULL;

	hret = h_get_24x7_catalog_page(page, 0, 0);
	if (hret)
		goto e_free;

	catalog_version_num = be_to_cpu(page_0->version);
	catalog_page_len = be_to_cpu(page_0->length);

	if (SIZE_MAX / 4096 < catalog_page_len) {
		pr_err("invalid page count: %zu\n", catalog_page_len);
		goto e_free;
	}

	catalog_len = catalog_page_len * 4096;

	event_entry_count = be_to_cpu(page_0->event_entry_count);
	event_data_offs   = be_to_cpu(page_0->event_data_offs);
	event_data_len    = be_to_cpu(page_0->event_data_len);

	pr_devel("cv %zu cl %zu eec %zu edo %zu edl %zu\n",
			(size_t)catalog_version_num, catalog_len, event_entry_count, event_data_offs, event_data_len);

	if ((SIZE_MAX / 4096 < event_data_len)
			|| (SIZE_MAX / 4096 < event_data_offs)
			|| (SIZE_MAX / 4096 - event_data_offs < event_data_len)) {
		pr_err("invalid event data offs %zu and/or len %zu\n",
				event_data_offs, event_data_len);
		goto e_free;
	}

	if ((event_data_offs + event_data_len) > catalog_page_len) {
		pr_err("event data %zu-%zu does not fit inside catalog 0-%zu\n",
				event_data_offs, event_data_offs + event_data_len,
				catalog_page_len);
		goto e_free;
	}

	if (SIZE_MAX / MAX_EVENTS_PER_EVENT_DATA - 1 < event_entry_count) {
		pr_err("event_entry_count %zu is invalid\n", event_entry_count);
		goto e_free;
	}

	/*
	 * extras: 1: NULL terminator
	 *         MAX_EVENTS_PER_EVENT_DATA: lets us catch overfilling this array.
	 */
#define ATTR_EXTRAS (MAX_EVENTS_PER_EVENT_DATA + 1)
	attr_max = event_entry_count * MAX_EVENTS_PER_EVENT_DATA + ATTR_EXTRAS;
	events = kmalloc_array(attr_max, sizeof(*events), GFP_KERNEL);
	if (!events) {
		pr_err("allocation of event attribute array failed\n");
		goto e_free;
	}

	pr_info("allocated space for %zu event attributes\n", attr_max);

	event_data_bytes = event_data_len * 4096;

	/*
	 * event data can span several pages, events can cross between these
	 * pages. Use vmalloc to make this easier.
	 */
	event_data = vmalloc(event_data_bytes);
	if (!event_data) {
		pr_err("could not allocate event data\n");
		goto e_events;
	}

	/*
	 * using vmalloc_to_phys() like this only works if PAGE_SIZE is
	 * divisible by 4096
	 */
	BUILD_BUG_ON(PAGE_SIZE % 4096);

	for (i = 0; i < event_data_len; i++) {
		hret = h_get_24x7_catalog_page_(vmalloc_to_phys(event_data + i * 4096),
				catalog_version_num, i + event_data_offs);
		if (hret) {
			pr_err("failed to get event data in page %zu\n", i + event_data_offs);
			goto e_event_data;
		}
	}

	end = event_data + event_data_bytes;
	event = event_data;
	junk_events = 0;
	event_ct = 0;

	/* Iterate over the catalog filling in the attribute vector */
	for (event_idx = 0; ; event_idx++) {
		size_t ev_len;
		void *ev_end, *calc_ev_end;
		size_t offset = (void *)event - (void *)event_data;
		if (offset >= event_data_bytes)
			break;

		if (event_ct >= (attr_max - ATTR_EXTRAS)) {
			pr_warn("too many event attributes needed\n");
			break;
		}

		if (event_idx >= event_entry_count) {
			pr_devel("catalog event data has %zu bytes of padding after last event\n",
					event_data_bytes - offset);
			break;
		}

		if (!event_fixed_portion_is_within(event, end)) {
			pr_warn("event %zu fixed portion is not within range\n", event_idx);
			break;
		}

		ev_len = be_to_cpu(event->length);

		if (ev_len % 16)
			pr_info("event %zu has length %zu not divisible by 16: event=%pK\n", event_idx, ev_len, event);

		ev_end = (__u8 *)event + ev_len;
		if (ev_end > end) {
			pr_warn("event %zu has .length=%zu, ends after buffer end: ev_end=%pK > end=%pK, offset=%zu\n",	event_idx, ev_len, ev_end, end, offset);
			break;
		}

		calc_ev_end = event_end(event, end);
		if (!calc_ev_end) {
			pr_warn("event %zu has a calculated length which exceeds buffer length %zu: event=%pK end=%pK, offset=%zu\n",
				event_idx, event_data_bytes, event, end, offset);
			break;
		}

		if (calc_ev_end > ev_end) {
			pr_warn("event %zu exceeds it's own length: event=%pK, end=%pK, offset=%zu, calc_ev_end=%pK\n",
				event_idx, event, ev_end, offset, calc_ev_end);
			break;
		}

		if (ev_len > 4096) {
			pr_warn("event %zu is %zu bytes, too large for us to handle. complain to the author.\n",
				event_idx + junk_events, ev_len);
			break;
		}

		if (event->event_group_record_len == 0) {
			pr_debug("invalid event, skipping\n");
			junk_events++;
			goto next_event;
		}

		ct = event_data_to_attrs(event_idx, events + event_ct, event);
		if (ct <= 0) {
			pr_warn("event %zu creation failure, skipping\n",
				event_idx);
			junk_events++;
		} else {
			event_ct += ct;
		}

next_event:
		event = (void *)event + ev_len;
	}

	if (event_idx != event_entry_count)
		pr_warn("event buffer ended before listed # of events were parsed (got %zu, wanted %zu)\n",
				event_idx, event_entry_count);

	pr_info("read %zu catalog entries, skipped %zu invalid events, created %zu event attrs\n",
			event_idx, junk_events, event_ct);

	events[event_ct] = NULL;

	vfree(event_data);
	kfree(page);
	return events;

e_event_data:
	vfree(event_data);
e_events:
	kfree(events);
e_free:
	kfree(page);
	return NULL;
}

static ssize_t catalog_read(struct file *filp, struct kobject *kobj,
			    struct bin_attribute *bin_attr, char *buf,
			    loff_t offset, size_t count)
{
	unsigned long hret;
	ssize_t ret = 0;
	size_t catalog_len = 0, catalog_page_len = 0, page_count = 0;
	loff_t page_offset = 0;
	uint64_t catalog_version_num = 0;
	void *page = kmem_cache_alloc(hv_page_cache, GFP_USER);
	struct hv_24x7_catalog_page_0 *page_0 = page;
	if (!page)
		return -ENOMEM;

	hret = h_get_24x7_catalog_page(page, 0, 0);
	if (hret) {
		ret = -EIO;
		goto e_free;
	}

	catalog_version_num = be64_to_cpu(page_0->version);
	catalog_page_len = be32_to_cpu(page_0->length);
	catalog_len = catalog_page_len * 4096;

	page_offset = offset / 4096;
	page_count  = count  / 4096;

	if (page_offset >= catalog_page_len)
		goto e_free;

	if (page_offset != 0) {
		hret = h_get_24x7_catalog_page(page, catalog_version_num,
					       page_offset);
		if (hret) {
			ret = -EIO;
			goto e_free;
		}
	}

	ret = read_offset_data(buf, count, offset,
				page, 4096, page_offset * 4096);
e_free:
	if (hret)
		pr_err("h_get_24x7_catalog_page(ver=%lld, page=%lld) failed:"
		       " rc=%ld\n",
		       catalog_version_num, page_offset, hret);
	kfree(page);

	pr_devel("catalog_read: offset=%lld(%lld) count=%zu(%zu) catalog_len=%zu(%zu) => %zd\n",
			offset, page_offset, count, page_count, catalog_len,
			catalog_page_len, ret);

	return ret;
}

#define PAGE_0_ATTR(_name, _fmt, _expr)				\
static ssize_t _name##_show(struct device *dev,			\
			    struct device_attribute *dev_attr,	\
			    char *buf)				\
{								\
	unsigned long hret;					\
	ssize_t ret = 0;					\
	void *page = kmem_cache_alloc(hv_page_cache, GFP_USER);	\
	struct hv_24x7_catalog_page_0 *page_0 = page;		\
	if (!page)						\
		return -ENOMEM;					\
	hret = h_get_24x7_catalog_page(page, 0, 0);		\
	if (hret) {						\
		ret = -EIO;					\
		goto e_free;					\
	}							\
	ret = sprintf(buf, _fmt, _expr);			\
e_free:								\
	kfree(page);						\
	return ret;						\
}								\
static DEVICE_ATTR_RO(_name)

PAGE_0_ATTR(catalog_version, "%lld\n",
		(unsigned long long)be64_to_cpu(page_0->version));
PAGE_0_ATTR(catalog_len, "%lld\n",
		(unsigned long long)be32_to_cpu(page_0->length) * 4096);
static BIN_ATTR_RO(catalog, 0/* real length varies */);

static struct bin_attribute *if_bin_attrs[] = {
	&bin_attr_catalog,
	NULL,
};

static struct attribute *if_attrs[] = {
	&dev_attr_catalog_len.attr,
	&dev_attr_catalog_version.attr,
	NULL,
};

static struct attribute_group if_group = {
	.name = "interface",
	.bin_attrs = if_bin_attrs,
	.attrs = if_attrs,
};

static const struct attribute_group *attr_groups[] = {
	&format_group,
	&event_group,
	&if_group,
	NULL,
};

static bool is_physical_domain(unsigned domain)
{
	return  domain == HV_PERF_DOMAIN_PHYSICAL_CHIP ||
		domain == HV_PERF_DOMAIN_PHYSICAL_CORE;
}

static unsigned long single_24x7_request(u8 domain, u32 offset, u16 ix,
					 u16 lpar, u64 *res,
					 bool success_expected)
{
	unsigned long ret;

	/*
	 * request_buffer and result_buffer are not required to be 4k aligned,
	 * but are not allowed to cross any 4k boundary. Aligning them to 4k is
	 * the simplest way to ensure that.
	 */
	struct reqb {
		struct hv_24x7_request_buffer buf;
		struct hv_24x7_request req;
	} __packed __aligned(4096) request_buffer = {
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
	} __packed __aligned(4096) result_buffer = {};

	ret = plpar_hcall_norets(H_GET_24X7_DATA,
			virt_to_phys(&request_buffer), sizeof(request_buffer),
			virt_to_phys(&result_buffer),  sizeof(result_buffer));

	if (ret) {
		if (success_expected)
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

static unsigned long event_24x7_request(struct perf_event *event, u64 *res,
		bool success_expected)
{
	return single_24x7_request(event_get_domain(event),
				event_get_offset(event),
				event_get_starting_index(event),
				event_get_lpar(event),
				res,
				success_expected);
}

static int h_24x7_event_init(struct perf_event *event)
{
	struct hv_perf_caps caps;
	unsigned domain;
	unsigned long hret;
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
		pr_devel("invalid domain %d\n", domain);
		return -EINVAL;
	}

	hret = hv_perf_caps_get(&caps);
	if (hret) {
		pr_devel("could not get capabilities: rc=%ld\n", hret);
		return -EIO;
	}

	/* PHYSICAL domains & other lpars require extra capabilities */
	if (!caps.collect_privileged && (is_physical_domain(domain) ||
		(event_get_lpar(event) != event_get_lpar_max()))) {
		pr_devel("hv permisions disallow: is_physical_domain:%d, lpar=0x%llx\n",
				is_physical_domain(domain),
				event_get_lpar(event));
		return -EACCES;
	}

	/* see if the event complains */
	if (event_24x7_request(event, &ct, false)) {
		pr_devel("test hcall failed\n");
		return -EIO;
	}

	return 0;
}

static u64 h_24x7_get_value(struct perf_event *event)
{
	unsigned long ret;
	u64 ct;
	ret = event_24x7_request(event, &ct, true);
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
}

static void h_24x7_event_stop(struct perf_event *event, int flags)
{
	h_24x7_event_update(event);
}

static int h_24x7_event_add(struct perf_event *event, int flags)
{
	if (flags & PERF_EF_START)
		h_24x7_event_start(event, flags);

	return 0;
}

static int h_24x7_event_idx(struct perf_event *event)
{
	return 0;
}

static struct pmu h_24x7_pmu = {
	.task_ctx_nr = perf_invalid_context,

	.name = "hv_24x7",
	.attr_groups = attr_groups,
	.event_init  = h_24x7_event_init,
	.add         = h_24x7_event_add,
	.del         = h_24x7_event_stop,
	.start       = h_24x7_event_start,
	.stop        = h_24x7_event_stop,
	.read        = h_24x7_event_update,
	.event_idx   = h_24x7_event_idx,
};

static int hv_24x7_init(void)
{
	int r;
	unsigned long hret;
	struct hv_perf_caps caps;

	if (!firmware_has_feature(FW_FEATURE_LPAR)) {
		pr_debug("not a virtualized system, not enabling\n");
		return -ENODEV;
	}

	hret = hv_perf_caps_get(&caps);
	if (hret) {
		pr_debug("could not obtain capabilities, not enabling, rc=%ld\n",
				hret);
		return -ENODEV;
	}

	hv_page_cache = kmem_cache_create("hv-page-4096", 4096, 4096, 0, NULL);
	if (!hv_page_cache)
		return -ENOMEM;

	event_group.attrs = create_events_from_catalog();

	r = perf_pmu_register(&h_24x7_pmu, h_24x7_pmu.name, -1);
	if (r)
		return r;

	return 0;
}

device_initcall(hv_24x7_init);
