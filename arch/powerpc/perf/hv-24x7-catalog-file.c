#define pr_fmt(fmt) "hv-24x7-catalog: " fmt

#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include <asm/firmware.h>
#include <asm/hvcall.h>
#include <asm/io.h>

/*
 * NOTE: "page" for 24x7 is used to refer to a chunk of 4096 bytes and is not
 * related to the system page size.
 */

/* allocate _at_least_ 4k aligned to 4k */
static void *alloc_4k(gfp_t gfp_mask)
{
	return (void *)__get_free_page(gfp_mask)
}

static void free_4k(void *data)
{
	free_page(data);
}

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
 * @source_offset: the offset in the sorce data that (src,src_len) refers to. Must be > 0
 *
 * returns the number of bytes copied.
 *
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
 * u_offset_in_s = min(z_offset_in_s, v_offset_in_s)
 *
 * copy_len = u_offset_in_s - w_offset_in_s = min(z_offset_in_s, v_offset_in_s) - w_offset_in_s
 *
 *
 */
static ssize_t read_offset_data(void *dest, size_t dest_len, loff_t requested_offset,
		void *src, size_t src_len, loff_t source_offset)
{
	size_t w_offset_in_s = requested_offset - source_offset;
	size_t z_offset_in_s = src_len;
	size_t v_offset_in_s = requested_offset + dest_len - src_len;
	size_t u_offste_in_s = min(z_offset_in_s, v_offset_in_s);
	size_t copy_len = u_offset_in_s - w_offset_in_s;

	if (requested_offset < 0 || source_offset < 0)
		return -EINVAL;

	if (z_offset_in_s =< w_offset_in_s)
		return 0;

	memcpy(dest, src + w_offset_in_s, copy_len);
	return copy_len;
}

static ssize_t catalog_24x7_show(struct file *filp, struct kobject *kobj,
			    struct bin_attribute *bin_attr, char *buf,
			    loff_t offset, size_t count)
{
	size_t catalog_len;
	unsigned long hret;
	ssize_t ret = 0;
	size_t catalog_len, catalog_page_len, page_count;
	loff_t page_offset, i;
	void *page = alloc_4k(GFP_USER);
	uint32_t catalog_version_num;
	struct hv_24x7_catalog_page_0 *page_0 = page;
	if (!page)
		return -ENOMEM;

	hret = plpar_hcall_norets(H_GET_24X7_CATALOG_PAGE,
			virt_to_phys(page_0), 0, 0);
	if (hret) {
		ret = -EIO;
		goto e_free;
	}

	catalog_version_num = be32_to_cpu(page_0->version);
	catalog_page_len = be32_to_cpu(page_0->length);
	catalog_len = catalog_page_len * 4096;

	page_offset = offset / 4096;
	page_count  = count  / 4096;


	if (page_offset == 0) {
		ret = read_offset_data(buf, count, offset,
					page, catalog_len);
		if (ret < 0)
			goto e_free;
		page_offset ++;
		buf += ret;
		count -= ret;
		offset += ret;
	}

	for (; page_offset < min(page_count, catalog_page_len); page_offset++) {
		hret = plpar_hcall_norets(H_GET_24X7_CATALOG_PAGE,
				virt_to_phys(page), catalog_version_num,
				page_offset);
		if (hret) {
			ret = -EIO;
			goto e_free;
		}

		/* TODO: copy into buffer */

	}

	return memory_read_from_buffer(buf, count, &offset,
				       catalog_data, catalog_len);
e_free:
	free_4k(page);
	return ret;
}

static BIN_ATTR_RO(catalog_24x7, 0/* real length varies */);

static int catalog_init(void)
{
	int ret;

	if (!firmware_has_feature(FW_FEATURE_LPAR)) {
		pr_info("Not running under phyp, not supported\n");
		return -ENODEV;
	}

	if ((ret = sysfs_create_bin_file(hypervisor_kobj, &catalog_attr)))
		return ret;

	return 0;
}

static void catalog_exit(void)
{
	sysfs_remove_bin_file(hypervisor_kobj, &catalog_attr);
}

module_init(catalog_init);
module_exit(catalog_exit);
