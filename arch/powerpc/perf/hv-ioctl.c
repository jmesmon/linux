


#define DRVNAME "hv-perf-raw"
#define pr_fmt(fmt) DRVNAME ": " fmt

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <asm/firmware.h>
#include <asm/io.h>
#include <asm/ioctl.h>
#include <asm/hvcall.h>

#include <uapi/asm/hv-ioctl.h>

static long hv_24x7_ioctl(__user struct hv_24x7_arg *user_arg)
{
	struct hv_24x7_arg arg;
	long hret;
	void *in, *out;
	int ret = copy_from_user(&arg, user_arg, sizeof(arg));
	if (ret)
		return ret;

	in = kzalloc(arg.in_sz, GFP_USER);
	if (!in)
		return -ENOMEM;
	out = kzalloc(arg.out_sz, GFP_USER);
	if (!out) {
		ret = -ENOMEM;
		goto e_in_alloc;
	}

	ret = copy_from_user(in, arg.input, arg.in_sz);
	if (ret)
		goto e_out_alloc;

	hret = plpar_hcall_norets(H_GET_24X7_DATA,
			virt_to_phys(in), arg.in_sz,
			virt_to_phys(out), arg.out_sz);
	ret = put_user(hret, &user_arg->hret);
	if (ret)
		goto e_out_alloc;

	ret = copy_to_user(arg.output, out, arg.out_sz);

e_out_alloc:
	kfree(out);
e_in_alloc:
	kfree(in);
	return ret;
}

static long hv_gpci_ioctl(__user struct hv_gpci_arg *user_arg)
{
	struct hv_gpci_arg arg;
	long hret;
	void *io;
	int ret = copy_from_user(&arg, user_arg, sizeof(arg));
	if (ret)
		return ret;

	io = kzalloc(arg.sz, GFP_USER);
	if (!io)
		return -ENOMEM;
	ret = copy_from_user(io, arg.data, arg.sz);
	if (ret)
		goto e_io_alloc;

	hret = plpar_hcall_norets(H_GET_PERF_COUNTER_INFO,
			virt_to_phys(io), arg.sz);
	ret = put_user(hret, &user_arg->hret);
	if (ret)
		goto e_io_alloc;

	ret = copy_to_user(arg.data, io, arg.sz);

e_io_alloc:
	kfree(io);
	return ret;
}

static long misc_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	__user void *user_arg = (__user void *)arg;
	switch (cmd) {
	case HV_GPCI_IOCTL:
		return hv_gpci_ioctl(user_arg);
	case HV_24X7_DATA_IOCTL:
		return hv_24x7_ioctl(user_arg);
#if 0
	case HV_24X7_CATALOG_IOCTL:
		return hv_24x7_catalog_ioctl(user_arg);
#endif
	default:
		 return -EINVAL;
	}
}

static int misc_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int misc_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations misc_fops = {
	.owner = THIS_MODULE,
	.open = misc_open,
	.release = misc_release,
	.unlocked_ioctl = misc_ioctl,
};

static struct miscdevice misc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = DRVNAME,
	.fops  = &misc_fops,
};

static int hv_gpci_init(void)
{
	int err;
	if (!firmware_has_feature(FW_FEATURE_LPAR))
		return -ENODEV;

	err = misc_register(&misc_dev);
	if (err) {
		pr_err("failed to register device\n");
		return err;
	}

	return 0;
}

static void hv_gpci_exit(void)
{
	misc_deregister(&misc_dev);
}

module_init(hv_gpci_init);
module_exit(hv_gpci_exit);
