#define DRVNAME "phyp-phys-layout"
#define pr_fmt(fmt) DRVNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/debugfs.h>

#include <asm/firmware.h>
#include <asm/h_counter_info.h>
#include <asm/hvcall.h>
#include <asm/io.h>

struct dentry *root;

/*
 * info:
 * how many hw_cpus?
 * which hw_cpus am I using?
 *
 * interconnects:
 * hw_cpu <-> hw_cpu
 *
 * mappings:
 * cpu -> hw_cpu
 * hw_cpu -> core
 * core -> chip
 *
 * attributes:
 * hw_cpu -> numa_node
 */

struct phyp_perf_caps {
	u16 version;
	u16 other_allowed:1,
	    ga:1,
	    expanded:1,
	    lab:1,
	    unused:12;
};

static unsigned long get_phyp_perf_caps(struct phyp_perf_caps *caps)
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

static int layout_init(void)
{
	struct phyp_perf_caps caps;
	unsigned long r;

	if (!firmware_has_feature(FW_FEATURE_LPAR)) {
		pr_info("Not running under phyp, not supported\n");
		return -ENODEV;
	}

	r = get_phyp_perf_caps(&caps);
	pr_info("caps: 0x%lx ver=0x%x other_allowed=%d ga=%d expanded=%d lab=%d\n",
			r, caps.version, caps.other_allowed, caps.ga, caps.expanded, caps.lab);

	root = debugfs_create_dir("phys-layout", NULL);
	if (!root)
		return 0;

	/* TODO: retreve the data, and export it */

	return 0;
}

static void layout_exit(void)
{
	debugfs_remove_recursive(root);
}

module_init(layout_init);
module_exit(layout_exit);
