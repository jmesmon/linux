#ifndef LINUX_POWERPC_UAPI_HV_IOCTL_H_
#define LINUX_POWERPC_UAPI_HV_IOCTL_H_

#include <linux/types.h>

struct hv_gpci_arg {
	void *data;
	__u64 sz;
	__u64 hret;
};
#define HV_GPCI_IOCTL _IOWR('c', 0xB0, struct hv_gpci_arg)

struct hv_24x7_arg {
	void *input, *output;
	__u64 in_sz, out_sz;
	__u64 hret;
};
#define HV_24X7_DATA_IOCTL _IOWR('c', 0xB1, struct hv_24x7_arg)



#endif
