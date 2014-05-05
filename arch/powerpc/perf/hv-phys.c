/*
 * Provides information about the physical machine if we are a virtualized guest.
 */

#define pr_fmt(fmt) "hv-phys: " fmt

#include <linux/init.h>

#include <linux/byteorder.h>

#include <asm/firmware.h>
#include <asm/hvcall.h>
#include <asm/io.h>

#include "hv-gpci.h"
#include "hv-common.h"

/*
 * Values for the 'detail_rc'
 */

/* Success */
#define GEN_BASE_SUCCESS 0x00000000
/* Bad buffer pointer */
#define GEN_PRIV_INVALID_ADDR 0x00000100
/* Invalid buffer length */
#define GEN_PRIV_INVALID_LEN 0x00000101
/* Buffer size cannot accommodate all the information, and a partial buffer was
 * returned */
#define GEN_BUF_TOO_SMALL 0x0000001B
/* Problem not defined by more specific return code */
#define GEN_HARDWARE_ERROR 0x00000200
/* The requested performance data is not available on this version of the
 * hardware or this version of the firmware. */
#define GEN_NOT_AVAILABLE 0x00000300

static void show_phys_info(void)
{
	unsigned long hret;
	unsigned i = 0;
	struct p {
		struct hv_get_perf_counter_info_params params;
		struct hv_gpci_dispatch_timebase_by_processor data[32];
	} __packed __aligned(sizeof(uint64_t));
	struct hv_gpci_dispatch_timebase_by_processor *dtbp;
	unsigned elem_size;

	struct p arg = {
		.params = {
			.counter_request = cpu_to_be32(
					HV_GPCI_dispatch_timebase_by_processor),
			.starting_index = cpu_to_be32(0),
			.counter_info_version_in = 0,
		}
	};

	hret = plpar_hcall_norets(H_GET_PERF_COUNTER_INFO,
			       virt_to_phys(&arg), sizeof(arg));

	if (hret == H_PARAMETER && be_to_cpu(arg.params.detail_rc) == GEN_BUF_TOO_SMALL) {
		pr_info("buffer too small, continuing anyway (%d returned values)\n",
				be_to_cpu(arg.params.returned_values));
	} else if (hret) {
		pr_info("hcall failure version_out=0x%x starting_index=%d secondary_index=%d returned_values=%d detail_rc=%x ret=%ld\n",
				be_to_cpu(arg.params.counter_info_version_out),
				be_to_cpu(arg.params.starting_index),
				be_to_cpu(arg.params.secondary_index),
				be_to_cpu(arg.params.returned_values),
				be_to_cpu(arg.params.detail_rc),
				hret);
		return;
	}

	elem_size = be_to_cpu(arg.params.cv_element_size);
	for (i = 0, dtbp = (void *)arg.params.counter_value; i < be_to_cpu(arg.params.returned_values); i++, dtbp = (void *)dtbp + elem_size) {
		/* TODO: check that dtbp is safely within the allocation size */
		pr_info("phys cpu: hw_proc_id=0x%x, owning_part_id=0x%x, state=0x%x, version=0x%x, hw_chip_id=0x%x,\n"
			"phys_module_id=0x%x 1_affin_domain_ix=0x%x 2_affin_domain_ix=0x%x proc_version=0x%x \n"
			"logical proc ix=0x%x proc_id_reg=0x%x phys_proc_idx=0x%x\n",
				be_to_cpu(dtbp->hw_processor_id),
				be_to_cpu(dtbp->owning_part_id),
				dtbp->processor_state,
				dtbp->version,
				be_to_cpu(dtbp->hw_chip_id),
				be_to_cpu(dtbp->phys_module_id),
				be_to_cpu(dtbp->primary_affinity_domain_idx),
				be_to_cpu(dtbp->secondary_affinity_domain_idx),
				be_to_cpu(dtbp->processor_version),
				be_to_cpu(dtbp->logical_processor_idx),
				be_to_cpu(dtbp->processor_id_register),
				be_to_cpu(dtbp->phys_processor_idx));
	}
}

static int hv_phys_init(void)
{
	unsigned long hret;
	struct hv_perf_caps caps;

	if (!firmware_has_feature(FW_FEATURE_LPAR)) {
		pr_info("not a virtualized system, not enabling\n");
		return -ENODEV;
	}

	hret = hv_perf_caps_get(&caps);
	if (hret) {
		pr_info("could not obtain capabilities, error 0x%80lx, not enabling\n",
				hret);
		return -ENODEV;
	}

	/* TODO: setup sysfs */
	show_phys_info();

	return 0;
}

device_initcall(hv_phys_init);
