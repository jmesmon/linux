#ifndef LINUX_POWERPC_PERF_HV_24X7_H_
#define LINUX_POWERPC_PERF_HV_24X7_H_

#include <linux/types.h>

struct hv_24x7_request {
	/* PHYSICAL domains require enabling via phyp/hmc. */
#define HV_24X7_PERF_DOMAIN_PHYSICAL_CHIP 0x01
#define HV_24X7_PERF_DOMAIN_PHYSICAL_CORE 0x02
#define HV_24X7_PERF_DOMAIN_VIRTUAL_PROCESSOR_HOME_CORE   0x03
#define HV_24X7_PERF_DOMAIN_VIRTUAL_PROCESSOR_HOME_CHIP   0x04
#define HV_24X7_PERF_DOMAIN_VIRTUAL_PROCESSOR_HOME_NODE   0x05
#define HV_24X7_PERF_DOMAIN_VIRTUAL_PROCESSOR_REMOTE_NODE 0x06
	__u8 performance_domain;
	__u8 reserved[0x1];

	/* bytes to read starting at @data_offset. must be a multiple of 8 */
	__be16 data_size;

	/*
	 * byte offset within the perf domain to read from. must be 8 byte
	 * aligned
	 */
	__be32 data_offset;

	/*
	 * only valid for VIRTUAL_PROCESSOR domains, ignored for others.
	 * -1 means "current partition only"
	 *  Enabling via phyp/hmc required for non-"-1" values. 0 forbidden
	 *  unless requestor is 0.
	 */
	__be16 starting_lpar_ix;

	/*
	 * Ignored when @starting_lpar_ix == -1
	 * Ignored when @performance_domain is not VIRTUAL_PROCESSOR_*
	 * -1 means "infinite" or all
	 */
	__be16 max_num_lpars;

	/* chip, core, or virtual processor based on @performance_domain */
	__be16 starting_ix;
	__be16 max_ix;
} __packed;

struct hv_24x7_request_buffer {
	/* 0 - ? */
	/* 1 - ? */
#define HV_24X7_IF_VERSION_CURRENT 0x01
	__u8 interface_version;
	__u8 num_requests;
	__u8 reserved[0xE];
	struct hv_24x7_request requests[];
} __packed;

struct hv_24x7_result_element {
	__be16 lpar_ix;

	/*
	 * represents the core, chip, or virtual processor based on the
	 * request's @performance_domain
	 */
	__be16 domain_ix;

	/* -1 if @performance_domain does not refer to a virtual processor */
	__be32 lpar_cfg_instance_id;

	/* size = @result_element_data_size of cointaining result. */
	__u8 element_data[];
} __packed;

struct hv_24x7_result {
	__u8 result_ix;

	/*
	 * 0 = not all result elements fit into the buffer, additional requests
	 *     required
	 * 1 = all result elements were returned
	 */
	__u8 results_complete;
	__be16 num_elements_returned;

	/* This is a copy of @data_size from the coresponding hv_24x7_request */
	__be16 result_element_data_size;
	__u8 reserved[0x2];

	/* WARNING: only valid for first result element due to variable sizes
	 *          of result elements */
	/* struct hv_24x7_result_element[@num_elements_returned] */
	struct hv_24x7_result_element elements[];
} __packed;

struct hv_24x7_data_result_buffer {
	/* See versioning for request buffer */
	__u8 interface_version;

	__u8 num_results;
	__u8 reserved[0x1];
	__u8 failing_request_ix;
	__be32 detailed_rc;
	__be64 cec_cfg_instance_id;
	__be64 catalog_version_num;
	__u8 reserved2[0x8];
	/* WARNING: only valid for the first result due to variable sizes of
	 *	    results */
	struct hv_24x7_result results[]; /* [@num_results] */
} __packed;

/* From document "24x7 Event and Group Catalog Formats Proposal" v0.14 */
struct hv_24x7_catalog_page_0 {
#define HV_24X7_CATALOG_MAGIC 0x32347837 /* "24x7" in ASCII */
	__be32 magic;
	__be32 length; /* In 4096 byte pages */
	__u8 reserved1[4];
	__be32 version;
	__u8 build_time_stamp[16]; /* "YYYYMMDDHHMMSS\0\0" */
	__u8 reserved2[32];
	__be16 schema_data_offs; /* in 4096 byte pages */
	__be16 schema_data_len;  /* in 4096 byte pages */
	__be16 schema_entry_count;
	__u8 reserved3[2];
	__be16 group_data_offs; /* in 4096 byte pages */
	__be16 group_data_len;  /* in 4096 byte pages */
	__be16 group_entry_count;
	__u8 reserved4[2];
	__be16 formula_data_offs; /* in 4096 byte pages */
	__be16 formula_data_len;  /* in 4096 byte pages */
	__be16 formula_entry_count;
	__u8 reserved5[2];
} __packed;

struct hv_24x7_event_data {
	__be16 length; /* in bytes, must be a multiple of 16 */
	__u8 reserved1[2];
	__u8 domain; /* Chip = 1, Core = 2 */
	__u8 reserved2[1];
	__be16 event_group_record_offs; /* in bytes, must be 8 byte aligned */
	__be16 event_group_record_len; /* in bytes */

	/* in bytes, offset from event_group_record */
	__be16 event_counter_offs;

	/* verified_state, unverified_state, caveat_state, broken_state, ... */
	__be32 flags;

	__be16 primary_group_ix;
	__be16 group_count;
	__be16 event_name_len;
	__u8 remainder[];
	/* __u8 event_name[event_name_len - 2]; */
	/* __be16 event_description_len; */
	/* __u8 event_desc[event_description_len - 2]; */
	/* __be16 detailed_desc_len; */
	/* __u8 detailed_desc[detailed_desc_len - 2]; */
} __packed;

struct hv_24x7_group_data {
	__be16 length; /* in bytes, must be multiple of 16 */
	__u8 reserved1[2];
	__be32 flags; /* undefined contents */
	__u8 domain; /* Chip = 1, Core = 2 */
	__u8 reserved2[1];
	__be16 event_group_record_offs;
	__be16 event_group_record_len;
	__u8 group_schema_ix;
	__u8 event_count; /* 1 to 16 */
	__be16 event_ixs;
	__be16 group_name_len;
	__u8 remainder[];
	/* __u8 group_name[group_name_len]; */
	/* __be16 group_desc_len; */
	/* __u8 group_desc[group_desc_len]; */
} __packed;

/* TODO: Schema Data */
/* TODO: Event Counter Group Record (see the PORE/SLW workbook) */

/* "Get Event Counter Group Record Schema hypervisor interface" */

enum hv_24x7_grs_field_enums {
	/* GRS_COUNTER_1 = 1
	 * GRS_COUNTER_2 = 2
	 * ...
	 * GRS_COUNTER_31 = 32 // FIXME: Doc issue.
	 */
	GRS_COUNTER_BASE = 1,
	GRS_COUNTER_LAST = 32,
	GRS_TIMEBASE_UPDATE = 48,
	GRS_TIMEBASE_FENCE = 49,
	GRS_UPDATE_COUNT = 50,
	GRS_MEASUREMENT_PERIOD = 51,
	GRS_ACCUMULATED_MEASUREMENT_PERIOD = 52,
	GRS_LAST_UPDATE_PERIOD = 53,
	GRS_STATUS_FLAGS = 54,
};

enum hv_24x7_grs_enums {
	GRS_CORE_SCHEMA_INDEX = 0,
};

struct hv_24x7_grs_field {
	__be16 field_enum;
	__be16 offs; /* in bytes, within Event Counter group record */
	__be16 length; /* in bytes */
	__be16 flags; /* presently unused */
} __packed;

struct hv_24x7_grs {
	__be16 length;
	__u8 reserved1[2];
	__be16 descriptor;
	__be16 version_id;
	__u8 reserved2[6];
	__be16 field_entry_count;
	__u8 field_entrys[];
} __packed;

struct hv_24x7_formula_data {
	__be32 length; /* in bytes, must be multiple of 16 */
	__u8 reserved1[2];
	__be32 flags; /* not yet defined */
	__be16 group;
	__u8 reserved2[6];
	__be16 name_len;
	__u8 remainder[];
	/* __u8 name[name_len]; */
	/* __be16 desc_len; */
	/* __u8 desc[name_len]; */
	/* __be16 formula_len */
	/* __u8 formula[formula_len]; */
} __packed;

/* Formula Syntax: ie, impliment a forth interpereter. */
/* need fast lookup of the formula names, event names, "delta-timebase",
 * "delta-cycles", "delta-instructions", "delta-seconds" */
/* operators: '+', '-', '*', '/', 'mod', 'rem', 'sqr', 'x^y' (XXX: pow? xor?),
 *            'rot', 'dup' */

#endif
