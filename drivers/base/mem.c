#include <linux/module.h>
#include <linux/init.h>
#include <linux/topology.h>
#include <linux/capability.h>
#include <linux/device.h>
#include <linux/memory.h>
#include <linux/kobject.h>
#include <linux/memory_hotplug.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/stat.h>
#include <linux/slab.h>

#define MEMORY_CLASS_NAME	"memory"
static struct bus_type memory_subsys = {
	.name = MEMORY_CLASS_NAME,
	.dev_name = MEMORY_CLASS_NAME,
};

#ifdef CONFIG_MEMORY_FAILURE
/*
 * Support for offlining pages of memory
 */

/* Soft offline a page */
static ssize_t
store_soft_offline_page(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int ret;
	u64 pfn;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (strict_strtoull(buf, 0, &pfn) < 0)
		return -EINVAL;
	pfn >>= PAGE_SHIFT;
	if (!pfn_valid(pfn))
		return -ENXIO;
	ret = soft_offline_page(pfn_to_page(pfn), 0);
	return ret == 0 ? count : ret;
}

/* Forcibly offline a page, including killing processes. */
static ssize_t
store_hard_offline_page(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int ret;
	u64 pfn;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (strict_strtoull(buf, 0, &pfn) < 0)
		return -EINVAL;
	pfn >>= PAGE_SHIFT;
	ret = memory_failure(pfn, 0, 0);
	return ret ? ret : count;
}

static DEVICE_ATTR(soft_offline_page, 0644, NULL, store_soft_offline_page);
static DEVICE_ATTR(hard_offline_page, 0644, NULL, store_hard_offline_page);

static __init int memory_fail_init(void)
{
	int err;

	err = device_create_file(memory_subsys.dev_root,
				&dev_attr_soft_offline_page);
	if (!err)
		err = device_create_file(memory_subsys.dev_root,
				&dev_attr_hard_offline_page);
	return err;
}
#else
static inline int memory_fail_init(void)
{
	return 0;
}
#endif

/*
 * Initialize the sysfs support for memory devices...
 */
int __init memory_dev_init(void)
{
	int ret;
	int err;

	ret = subsys_system_register(&memory_subsys, NULL);
	if (ret)
		goto out;

	err = memory_fail_init();
	if (!ret)
		ret = err;
out:
	if (ret)
		printk(KERN_ERR "%s() failed: %d\n", __func__, ret);
	return ret;
}
