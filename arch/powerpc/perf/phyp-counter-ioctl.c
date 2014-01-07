

#define DRVNAME "24x7"
#define pr_fmt(fmt) DRVNAME ": " fmt

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <asm/firmware.h>
#include <asm/ioctl.h>
#include <asm/hvcall.h>
#include <asm/h_counter_info.h>

static long misc_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case COUNTER_INFO_IOCTL: {
		struct counter_info_arg ciarg;
		unsigned long rets[PLPAR_HCALL_BUFSIZE];
		long hret;
		struct phyp_perf_counter_info_params *params;
		int ret = copy_from_user(&ciarg, (__user void *)arg, sizeof(ciarg));
		if (ret)
			return ret;

		/* this HCALL requires the params be _less_ than 4096 bytes */
		if (ciarg.bytes >= 4096)
			return -EINVAL;

		params = kzalloc(ciarg.bytes, GFP_USER);
		ret = copy_from_user(params, ciarg.params, ciarg.bytes);
		if (ret)
			return ret;

		/* TODO: be more paranoid about capabilities */
		hret = plpar_hcall(H_GET_PERF_COUNTER_INFO, rets, params, ciarg.bytes);

		pr_info("hcall ret: 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx\n",
				hret, rets[0], rets[1], rets[2], rets[3]);

		ret = copy_to_user(ciarg.params, params, ciarg.bytes);

		kfree(params);
		return ret;
	}
	default:
		return -EINVAL;

	}

	return 0;
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

static int count_24x7_init(void)
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

static void count_24x7_exit(void)
{
	misc_deregister(&misc_dev);
}

module_init(count_24x7_init);
module_exit(count_24x7_exit);
