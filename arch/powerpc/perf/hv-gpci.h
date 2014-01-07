#ifndef LINUX_POWERPC_PERF_HV_GPCI_H_
#define LINUX_POWERPC_PERF_HV_GPCI_H_

#include <linux/types.h>

/* From the document "H_GetPerformanceCounterInfo Interface" v1.07 */

/* H_GET_PERF_COUNTER_INFO argument */
struct hv_get_perf_counter_info_params {
	__be32 counter_request; /* I */
	__be32 starting_index;  /* IO */
	__be16 secondary_index; /* IO */
	__be16 returned_values; /* O */
	__be32 detail_rc; /* O, only needed when called via *_norets() */

	/*
	 * O, size each of counter_value element in bytes, only set for version
	 * >= 0x3
	 */
	__be16 cv_element_size;

	/* I, 0 (zero) for versions < 0x3 */
	__u8 counter_info_version_in;

	/* O, 0 (zero) if version < 0x3. Must be set to 0 when making hcall */
	__u8 counter_info_version_out;
	__u8 reserved[0xC];
	__u8 counter_value[];
} __packed;

/*
 * counter info version => fw version/reference (spec version)
 *
 * 8 => power8 (1.07)
 * [7 is skipped by spec 1.07]
 * 6 => TLBIE (1.07)
 * 5 => v7r7m0.phyp (1.05)
 * [4 skipped]
 * 3 => v7r6m0.phyp (?)
 * [1,2 skipped]
 * 0 => v7r{2,3,4}m0.phyp (?)
 */
#define COUNTER_INFO_VERSION_CURRENT 0x8

/*
 * These determine the counter_value[] layout and the meaning of starting_index
 * and secondary_index.
 *
 * Unless otherwise noted, @secondary_index is unused and ignored.
 */
enum counter_info_requests {

	/* GENERAL */

	/* @starting_index: "starting" physical processor index or -1 for
	 *                  current physical processor. Data is only collected
	 *                  for the processors' "primary" thread.
	 */
	CIR_DISPATCH_TIMEBASE_BY_PROCESSOR = 0x10,

	/* @starting_index: starting partition id or -1 for the current logical
	 *                  partition (virtual machine).
	 */
	CIR_ENTITLED_CAPPED_UNCAPPED_DONATED_IDLE_TIMEBASE_BY_PARTITION = 0x20,

	/* @starting_index: starting partition id or -1 for the current logical
	 *                  partition (virtual machine).
	 */
	CIR_RUN_INSTRUCTIONS_RUN_CYCLES_BY_PARTITION = 0X30,

	/* @starting_index: must be -1 (to refer to the current partition)
	 */
	CIR_SYSTEM_PERFORMANCE_CAPABILITIES = 0X40,


	/* Data from this should only be considered valid if
	 * counter_info_version >= 0x3
	 * @starting_index: starting hardware chip id or -1 for the current hw
	 *		    chip id
	 */
	CIR_PROCESSOR_BUS_UTILIZATION_ABC_LINKS = 0X50,

	/* Data from this should only be considered valid if
	 * counter_info_version >= 0x3
	 * @starting_index: starting hardware chip id or -1 for the current hw
	 *		    chip id
	 */
	CIR_PROCESSOR_BUS_UTILIZATION_WXYZ_LINKS = 0X60,

	/*
	 * EXPANDED - the following are only avaliable if the CV_CM_EXPANDED
	 * bit is set from system_performace_capabilities. Enforcement is left
	 * to the hypervisor.
	 */

	/* Available if counter_info_version >= 0x3
	 * @starting_index: starting hardware chip id or -1 for the current hw
	 *		    chip id
	 */
	CIR_PROCESSOR_BUS_UTILIZATION_GX_LINKS = 0X70,

	/* Available if counter_info_version >= 0x3
	 * @starting_index: starting hardware chip id or -1 for the current hw
	 *		    chip id
	 */
	CIR_PROCESSOR_BUS_UTILIZATION_MC_LINKS = 0X80,

	/* Available if counter_info_version >= 0x3
	 * @starting_index: starting physical processor or -1 for the current
	 *                  physical processor
	 */
	CIR_PROCESSOR_CONFIG = 0X90,

	/* Available if counter_info_version >= 0x3
	 * @starting_index: starting physical processor or -1 for the current
	 *                  physical processor
	 */
	CIR_CURRENT_PROCESSOR_FREQUENCY = 0X91,

	/* Available if counter_info_version >= 0x3 and <= 0x7
	 * @starting_index: starting physical processor or -1 for the current
	 *		    physical processor
	 */
	CIR_PROCESSOR_CORE_UTILIZATION = 0X94,

	/* Available if counter_info_version >= 0x3
	 * @starting_index: starting partition id or -1 for the current logical
	 *		    partition
	 */
	CIR_PROCESSOR_CORE_POWER_MODE = 0X95,

	/* Available if counter_info_version >= 0x3
	 * @starting_index: starting partition id or -1 for the current
	 *		    partition
	 * @secondary_index: starting virtual processor index or -1 for the
	 *		     current virtual processor
	 *
	 * Note: -1 for @starting_index and @secondary_index is only allowed if
	 * both are -1
	 */
	CIR_AFFINITY_DOMAIN_INFORMATION_BY_VIRUTAL_PROCESSOR = 0XA0,

	/* Available if counter_info_version >= 0x3
	 * @starting_index: primary affinity domain index or -1 for current
	 *                  primary affinity domain (as determined by the
	 *                  physical processor).
	 */
	CIR_AFFINITY_DOMAIN_INFO_BY_DOMAIN = 0XB0,

	/* Available if counter_info_version >= 0x3
	 * @starting_index: partition id or -1 for current partition
	 */
	CIR_AFFINITY_DOMAIN_INFO_BY_PARTITION = 0XB1,

	/* Available if counter_info_version >= 0x3
	 * @starting_index: unused
	 */
	CIR_PHYSICAL_MEMORY_INFO = 0XC0,

	/* Available if counter_info_version >= 0x3
	 * @starting_index: starting hw chip id or -1 for current hw chip
	 */
	CIR_PROCESSOR_BUS_TOPOLOGY = 0XD0,

	/* Available if counter_info_version >= 0x5
	 * @starting_index: starting partition id or -1 for current
	 */
	CIR_PARTITION_HYPERVISOR_QUEUING_TIMES = 0XE0,

	/* Available if counter_info_version >= 0x5
	 * @starting_index: unused
	 */
	CIR_SYSTEM_HYPERVISOR_TIMES = 0XF0,

	/* Available if counter_info_version >= 0x6
	 * @starting_index: unused
	 */
	CIR_SYSTEM_TLBIE_COUNT_AND_TIME = 0xF4,

	/* Available if counter_info_version >= 0x8
	 * @starting_index: starting partition id or -1 for current
	 */
	CIR_PARTITION_INSTURCTION_COUNT_AND_TIME = 0X100,

	/* LAB */

	/* Available if counter_info_version < 0x8
	 * @starting_index: unused
	 */
	CIR_SET_MMCRH = 0X80001000,

	/* Available if counter_info_version < 0x8
	 * @starting_index: starting physical processor id or -1 for current
	 */
	CIR_RETRIEVE_HPMCX = 0X80002000,
};

/* counter value layout */
struct cv_dispatch_timebase_by_processor {
	__be64 processor_time_in_timebase_cycles;
	__be32 hw_processor_id;
	__be16 owning_part_id; /* 0xffff if shared or unowned */
	__u8 processor_state;
	__u8 version; /* unused unless counter_info_version == 0 */
	__be32 hw_chip_id; /* -1 for "Not Installed" processors */
	__be32 phys_module_id; /* -1 for "Not Installed" processors */
	__be32 primary_affinity_domain_idx;
	__be32 secondary_affinity_domain_idx;
	__be32 processor_version;
	__be16 logical_processor_idx;
	__u8 reserved[0x2];

	/* counter_info_version >= 0x3 || version >= 0x1 */
	__be32 processor_id_register;
	__be32 phys_processor_idx; /* counter_info_version >= 0x3 */
} __packed;

struct cv_entitled_capped_uncapped_donated_idle_timebase_by_partition {
	__be64 partition_id;
	__be64 entitled_cycles;
	__be64 consumed_capped_cycles;
	__be64 consumed_uncapped_cycles;
	__be64 cycles_donated;
	__be64 purr_idle_cycles;
} __packed;

struct cv_run_instructions_run_cycles_by_partition {
	__be64 partition_id;
	__be64 instructions_completed; /* 0 if collection is unsupported */
	__be64 cycles; /* 0 if collection is unsupported */
} __packed;

struct cv_system_performance_capabilities {
	/* If != 0, allowed to collect data from other partitions */
	__u8 perf_collect_privileged;

	/* These following are only valid if counter_info_version >= 0x3 */
#define CV_CM_GA       0x1
#define CV_CM_EXPANDED 0x2
#define CV_CM_LAB      0x4
	/* remaining bits are reserved */
	__u8 capability_mask;
	__u8 reserved[0xE];
} __packed;

struct cv_processor_bus_utilization_abc_links {
	__be32 hw_chip_id;
	__u8 reserved1[0xC];
	__be64 total_link_cycles;
	__be64 idle_cycles_a;
	__be64 idle_cycles_b;
	__be64 idle_cycles_c;
	__u8 reserved2[0x20];
} __packed;

struct cv_processor_bus_utilization_wxyz_links {
	__be32 hw_chip_id;
	__u8 reserved1[0xC];
	__be64 total_link_cycles;

	/* Inactive links (all cycles idle) give -1 */
	__be64 idle_cycles_w;
	__be64 idle_cycles_x;
	__be64 idle_cycles_y;
	__be64 idle_cycles_z;
	__u8 reserved2[0x28];
} __packed;

/* EXPANDED */

struct cv_gx_cycles {
	__be64 address_cycles;
	__be64 data_cycles;
	__be64 retries;
	__be64 bus_cycles;
	__be64 total_cycles;
} __packed;

struct cv_gx_cycles_io {
	struct cv_gx_cycles in, out;
} __packed;

struct cv_processor_bus_utilization_gx {
	__be32 hw_chip_id;
	__u8 reserved1[0xC];
	struct cv_gx_cycles_io gx[2];
} __packed;

struct cv_mc_counts {
	__be64 frames;
	__be64 reads;
	__be64 writes;
	__be64 total_cycles;
} __packed;

/* inactive links return 0 for all utilization data */
struct cv_processor_bus_utilization_mc_links {
	__be32 hw_chip_id;
	__u8 reserved1[0xC];
	struct cv_mc_counts mc[2];
} __packed;

struct cv_processor_config {
	__be32 phys_processor_idx;
	__be32 hw_node_id;
	__be32 hw_card_id;
	__be32 phys_module_id;
	__be32 hw_chip_id;
	__be32 hw_processor_id;
	__be32 processor_id_register;

#define CV_PS_NOT_INSTALLED 0x1
#define CV_PS_GAURDED_OFF   0x2
#define CV_PS_UNLICENSED    0x3
#define CV_PS_SHARED        0x4
#define CV_PS_BORROWED      0x5
#define CV_PS_DEDICATED     0x6
	__u8 processor_state;

	__u8 reserved1[0x1];
	__be16 owning_part_id;
	__be32 processor_version;
	__u8 reserved2[0x4];
} __packed;

struct cv_current_processor_frequency {
	__be32 phys_processor_idx;
	__be32 hw_processor_id;
	__u8 reserved1[0x8];
	__be32 nominal_freq_mhz;
	__be32 current_freq_mhz;
	__u8 reserved2[0x8];
} __packed;

struct cv_processor_core_utilization {
	__be32 phys_processor_idx;
	__be32 hw_processor_id;
	__be64 cycles;
	__be64 timebase_at_collection;
	__be64 purr_cycles;
	__be64 sum_of_cycles_across_threads;
	__be64 instructions_completed;
} __packed;

struct cv_processor_core_power_mode {
	__be16 partition_id;
	__u8 reserved1[0x6];

#define CV_PM_NONE		 0x0
#define CV_PM_NOMINAL		 0x1
#define CV_PM_DYNAMIC_MAX_PERF   0x2
#define CV_PM_DYNAMIC_POWER_SAVE 0x3
#define CV_PM_UNKNOWN		 0xF
	__be16 power_mode;

	__u8 reserved2[0x6];
} __packed;

struct cv_affinity_domain_information_by_virutal_processor {
	__be16 partition_id;
	__be16 virtual_processor_idx;
	__u8 reserved1[0xC];
	__be16 phys_processor_idx;
	__be16 primary_affinity_domain_idx;
	__be16 secondary_affinity_domain_idx;
	__u8 reserved2[0x2];
	__u8 reserved3[0x8];
} __packed;

struct cv_affinity_domain_info_by_domain {
	__be16 primary_affinity_domain_idx;
	__be16 secondary_affinity_domain_idx;
	__be32 total_processor_units;
	__be32 free_dedicated_processor_units;
	__be32 free_shared_processor_units;
	__be32 total_memory_lmbs;
	__be32 free_memory_lmbs;
	__be32 num_partitions_in_domain;
	__u8 reserved1[0x14];
} __packed;

struct cv_affinity_domain_info_by_partition {
	__be16 partition_id;
	__u8 reserved1[0x6];
	__be16 assignment_order;

#define CV_PPS_UNKNOWN			      0x00
#define CV_PPS_CONTAIN_IN_PRIMARY_DOMAIN      0x01
#define CV_PPS_CONTAIN_IN_SECONDARY_DOMAIN    0x02
#define CV_PPS_SPREAD_ACROSS_SECONDAY_DOMAINS 0x03
#define CV_PPS_WHEREEVER		      0x04
#define CV_PPS_SCRAMBLE			      0x05
	__u8 partition_placement_spread;

	__u8 parition_affinity_score;
	__be16 num_affinity_domain_elements;
	__be16 affinity_domain_element_size;
	__u8 domain_elements[];
} __packed;

struct cv_affinity_domain_elem {
	__be16 primary_affinity_domain_idx;
	__be16 secondary_affinity_domain_idx;
	__be32 dedicated_processor_units_allocated;
	__be32 dedicated_memory_allocated_reserved_1;
	__be32 dedicated_memory_allocated_reserved_2;
	__be32 dedicated_memory_allocated_16Gb_pages;
	__u8 reserved[0x8];
} __packed;

/* Also available via `of_get_flat_dt_prop(node, "ibm,lmb-size", &l)` */
struct cv_physical_memory_info {
	__be64 lmb_size_in_bytes;
	__u8 reserved1[0x18];
} __packed;

struct cv_processor_bus_topology {
	__be32 hw_chip_id;
	__be32 hw_node_id;
	__be32 fabric_chip_id;
	__u8 reserved1[0x4];

#define CV_IM_A_LINK_ACTIVE (1 << 0)
#define CV_IM_B_LINK_ACTIVE (1 << 1)
#define CV_IM_C_LINK_ACTIVE (1 << 2)
/* Bits 3-5 are reserved */
#define CV_IM_ABC_LINK_WIDTH_MASK ((1 << 6) | (1 << 7))
#define CV_IM_ABC_LINK_WIDTH_SHIFT 6
#define CV_IM_ABC_LINK_WIDTH_8B 0x0
#define CV_IM_ABC_LINK_WIDTH_4B 0x1

#define CV_IM_W_LINK_ACTIVE (1 << 8)
#define CV_IM_X_LINK_ACTIVE (1 << 9)
#define CV_IM_Y_LINK_ACTIVE (1 << 10)
#define CV_IM_Z_LINK_ACTIVE (1 << 11)
/* Bits 12-13 are reserved */

#define CV_IM_WXYZ_LINK_WIDTH_MASK ((1 << 14) | (1 << 15))
#define CV_IM_WXYZ_LINK_WIDTH_SHIFT 14
#define CV_IM_WXYZ_LINK_WIDTH_8B 0x0
#define CV_IM_WXYZ_LINK_WIDTH_4B 0x1

#define CV_IM_GX0_CONFIGURED (1 << 16)
#define CV_IM_GX1_CONFIGURED (1 << 17)
/* Bits 18-23 are reserved */
#define CV_IM_MC0_CONFIGURED (1 << 24)
#define CV_IM_MC1_CONFIGURED (1 << 25)
/* Bits 26-31 are reserved */

	__be32 info_mask;

	__u8 hw_node_id_connected_to_a_link;
	__u8 hw_node_id_connected_to_b_link;

	__u8 reserved2[0x2];

	__u8 fabric_chip_id_connected_to_w_link;
	__u8 fabric_chip_id_connected_to_x_link;
	__u8 fabric_chip_id_connected_to_y_link;
	__u8 fabric_chip_id_connected_to_z_link;

	__u8 reserved3[0x4];
} __packed;

struct cv_partition_hypervisor_queuing_times {
	__be16 partition_id;
	__u8 reserved1[0x6];
	__be64 time_waiting_for_entitlement; /* in timebase cycles */
	__be64 times_waited_for_entitlement;
	__be64 time_waiting_for_phys_processor; /* in timebase cycles */
	__be64 times_waited_for_phys_processor;
	__be64 dispatches_on_home_processor_core;
	__be64 dispatches_on_home_primary_affinity_domain;
	__be64 dispatches_on_home_secondary_affinity_domain;
	__be64 dispatches_off_home_secondary_affinity_domain;
	__be64 dispatches_on_dedicated_processor_donating_cycles;
} __packed;

struct cv_system_hypervisor_times {
	__be64 phyp_time_spent_to_dispatch_virtual_processors;
	__be64 phyp_time_spent_processing_virtual_processor_timers;
	__be64 phyp_time_spent_managing_partitions_over_entitlement;
	__be64 time_spent_on_system_managment;
} __packed;

struct cv_system_tlbie_count_and_time {
	__be64 tlbie_instructions_issued;
	__be64 time_spent_issuing;
} __packed;

struct cv_partition_instruction_count_and_time {
	__be16 partition_id;
	__u8 reserved1[0x6];
	__be64 instructions_performed;
	__be64 time_collected;
} __packed;

/* LAB */

struct cv_set_mmcrh {
	/*
	 * Only HPMC bits (40:46, 48:54) used, all others ignored
	 * -1 = default (0x00000000_003C1200)
	 */
	__be64 mmcrh_value_to_set;
} __packed;

struct cv_retrieve_hpmcx {
	__be32 hw_processor_id;
	__u8 reserved1[0x4];
	__be64 mmcrh_current;
	__be64 time_since_mmcrh_was_set;
	__be64 hpmc1_since_current_mmcrh;
	__be64 hpmc2_since_current_mmcrh;
	__be64 hpmc3_since_current_mmcrh;
	__be64 hpmc3_current;
	__be64 hpmc4_since_current_mmcrh;
	__be64 hpmc4_current;
} __packed;

#endif
