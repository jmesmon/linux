#define pr_fmt(fmt) "hv-24x7-catalog: " fmt

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include <asm/firmware.h>
#include <asm/hvcall.h>
#include <asm/io.h>

static struct dentry *root;
static void *p;
static struct debugfs_blob_wrapper blob;

static void *get_4k(void)
{
	if (PAGE_SIZE > 4096)
		return kzalloc(GFP_KERNEL, 4096);
	else if (PAGE_SIZE == 4096)
		return (void *)__get_free_page(GFP_KERNEL);
	else {
		WARN(1, "Unhandled PAGE_SIZE 0x%lx\n", PAGE_SIZE);
		return NULL;
	}
}

static void free_4k(void *p)
{
	if (PAGE_SIZE > 4096)
		kfree(p);
	else if (PAGE_SIZE == 4096)
		free_page((unsigned long)p);
	else
		WARN(1, "Unhandled PAGE_SIZE 0x%lx\n", PAGE_SIZE);
}

static int dbg_init(void)
{
	long ret;

	if (!firmware_has_feature(FW_FEATURE_LPAR)) {
		pr_info("Not running under phyp, not supported\n");
		return -ENODEV;
	}

	root = debugfs_create_dir("phyp", NULL);
	if (!root)
		return -ENODEV;

	p = get_4k();
	if (!p) {
		ret = -ENOMEM;
		goto e_dir;
	}

	ret = plpar_hcall_norets(H_GET_24X7_CATALOG_PAGE,
			virt_to_phys(p), 0, 0);
	if (ret) {
		pr_err("Could not get 24x7 catalog page: 0x%lx\n", ret);
		ret = -EINVAL;
		goto e_mem;
	}

	blob.data = p;
	blob.size = 4096;

	debugfs_create_blob("24x7_catalog", 0444, root, &blob);
	return 0;

e_mem:
	free_4k(p);
e_dir:
	debugfs_remove_recursive(root);
	return ret;
}

static void dbg_exit(void)
{
	free_4k(p);
	debugfs_remove_recursive(root);
}

module_init(dbg_init);
module_exit(dbg_exit);
