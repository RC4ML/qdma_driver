#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/rwsem.h>
#include <linux/pgtable.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <asm/io.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/page_types.h>
#include "rc4ml.h"

MODULE_LICENSE("GPL");

static dev_t rc4ml_devno;
static struct cdev rc4ml_cdev;
static struct class *cls;
static struct device *rc4ml_device;
struct rc4ml_file_ctx
{
	struct huge_table_t huge_table;
	struct mutex huge_lock;
};
static int a;
size_t bridge_phy_addr;

static int rc4ml_open(struct inode *inode, struct file *pfile)
{
	struct rc4ml_file_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (ctx == NULL)
	{
		return -ENOMEM;
	}
	mutex_init(&ctx->huge_lock);
	pfile->private_data = ctx;
	printk("rc4ml dev opened\n");
	return 0;
}

static void rc4ml_release_huge_table(struct huge_table_t *table)
{
	unsigned long i;
	unsigned long npages = table->size / PAGE_SIZE;

	if (table->huge_pages != NULL)
	{
		for (i = 0; i < npages; i++)
		{
			put_page(table->huge_pages[i]);
		}
		vfree(table->huge_pages);
	}
	memset(table, 0, sizeof(*table));
}

static int rc4ml_release(struct inode *inode, struct file *pfile)
{
	struct rc4ml_file_ctx *ctx = pfile->private_data;

	if (ctx != NULL)
	{
		mutex_lock(&ctx->huge_lock);
		rc4ml_release_huge_table(&ctx->huge_table);
		mutex_unlock(&ctx->huge_lock);
		kfree(ctx);
		pfile->private_data = NULL;
	}
	printk("rc4ml dev released\n");
	return 0;
}

static int ioctl_huge_set(struct rc4ml_file_ctx *ctx, unsigned long arg)
{
	long rc;
	int i;
	int j = 0;
	struct huge_mem buf;
	unsigned long npages;
	unsigned long paddr;
	unsigned long last_paddr = 0;
	struct huge_table_t new_table = {};

	rc = copy_from_user(&buf, (struct huge_mem *)arg, sizeof(buf));
	if (rc != 0)
	{
		return -EFAULT;
	}
	if (buf.size == 0)
	{
		return -EINVAL;
	}

	npages = 1 + (buf.size - 1) / PAGE_SIZE;
	printk(KERN_INFO "req npages %ld\n", npages);
	printk(KERN_INFO "huge addr %px, size %ld\n", (void *)buf.vaddr, buf.size);
	new_table.huge_pages = vmalloc(npages * sizeof(*new_table.huge_pages));
	if (new_table.huge_pages == NULL)
	{
		return -ENOMEM;
	}

#ifndef MMAP_LOCK_INITIALIZER
    down_read(&current->mm->mmap_sem);
#else
    mmap_read_lock(current->mm);
#endif
    rc = get_user_pages(buf.vaddr, npages, 1, new_table.huge_pages, NULL);
#ifndef MMAP_LOCK_INITIALIZER
    up_read(&current->mm->mmap_sem);
#else
    mmap_read_unlock(current->mm);
#endif
	if (rc != npages)
	{
		printk(KERN_INFO "get_user_pages failed: requested %ld, received %ld\n", npages, rc);
		if (rc > 0)
		{
			for (i = 0; i < rc; i++)
			{
				put_page(new_table.huge_pages[i]);
			}
		}
		vfree(new_table.huge_pages);
		return rc < 0 ? rc : -EFAULT;
	}

	for (i = 0; i < npages; i++)
	{
		paddr = page_to_phys(new_table.huge_pages[i]);
		if (paddr - last_paddr != PAGE_SIZE)
		{
			printk(KERN_INFO "i:%d %px delta:%ld\n", i, (void *)paddr, paddr - last_paddr);
		}
		last_paddr = paddr;
		if (i % 512 == 0)
		{
			printk(KERN_INFO "huge paddr: %px\n", (void *)paddr);
			j++;
		}
	}
	new_table.size = npages * PAGE_SIZE;
	new_table.nhpages = j;
	new_table.vaddr_start = buf.vaddr;

	rc4ml_release_huge_table(&ctx->huge_table);
	ctx->huge_table = new_table;
	printk(KERN_INFO "IOCTL_BUFFER_SET\n");
	return 0;
}

static int ioctl_huge_get(struct rc4ml_file_ctx *ctx, unsigned long arg)
{
	unsigned long rc;
	int i;
	struct huge_mapping map;
	unsigned long *phy_addr;
	struct huge_table_t *huge_table = &ctx->huge_table;

	if (huge_table->huge_pages == NULL)
	{
		return -EINVAL;
	}
	rc = copy_from_user(&map, (struct huge_mapping *)arg, sizeof(map));
	if (rc != 0)
	{
		return -EFAULT;
	}
	printk(KERN_INFO "map huge page number:%ld\n", map.nhpages);
	map.nhpages = huge_table->nhpages;
	phy_addr = kmalloc(sizeof(*phy_addr) * map.nhpages, GFP_KERNEL);
	if (phy_addr == NULL)
	{
		return -ENOMEM;
	}
	for (i = 0; i < map.nhpages; i++)
	{
		phy_addr[i] = page_to_phys(huge_table->huge_pages[i * 512]);
	}
	if (copy_to_user((struct huge_mapping *)arg, &map, sizeof(map)))
	{
		kfree(phy_addr);
		return -EFAULT;
	}
	if (copy_to_user(map.phy_addr, phy_addr, sizeof(*phy_addr) * map.nhpages))
	{
		kfree(phy_addr);
		return -EFAULT;
	}

	printk(KERN_INFO "IOCTL_BUFFER_GET\n");
	kfree(phy_addr);
	return 0;
}

static long rc4ml_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	struct rc4ml_file_ctx *ctx = f->private_data;

	printk("ioctl called.\n");
	switch (cmd)
	{
	case QUERY_GET_VALUE:
		rc = copy_to_user((int *)arg, &a, sizeof(int)) ? -EFAULT : 0;
		break;
	case QUERY_CLEAR_VALUE:
		a = 0;
		break;
	case QUERY_SET_VALUE:
		rc = copy_from_user(&a, (int *)arg, sizeof(int)) ? -EFAULT : 0;
		break;
	case HUGE_MAPPING_SET:
	case HUGE_MAPPING_GET:
		if (ctx == NULL)
		{
			return -EINVAL;
		}
		mutex_lock(&ctx->huge_lock);
		if (cmd == HUGE_MAPPING_SET)
		{
			rc = ioctl_huge_set(ctx, arg);
		}
		else
		{
			rc = ioctl_huge_get(ctx, arg);
		}
		mutex_unlock(&ctx->huge_lock);
		break;
	default:
		printk(KERN_ERR "Unknown ioctl command: %u\n", cmd);
		return -EINVAL;
	}
	return rc;
}
static struct file_operations rc4ml_ops = {
	.open = rc4ml_open,
	.release = rc4ml_release,
	.unlocked_ioctl = rc4ml_ioctl,
};

int rc4ml_init(void)
{
	int res;

	res = alloc_chrdev_region(&rc4ml_devno, rc4ml_minor, 1, "rc4ml_dev");
	if (res < 0)
	{
		printk(KERN_ALERT "rc4ml alloc_chrdev_region failed.\n");
		return res;
	}

	cdev_init(&rc4ml_cdev, &rc4ml_ops);
	rc4ml_cdev.owner = THIS_MODULE;
	res = cdev_add(&rc4ml_cdev, rc4ml_devno, 1);
	if (res < 0)
	{
		printk(KERN_ALERT "rc4ml cdev_add failed.\n");
		unregister_chrdev_region(rc4ml_devno, 1);
		return res;
	}

	cls = class_create(THIS_MODULE, "rc4ml_class");
	if (IS_ERR(cls))
	{
		res = PTR_ERR(cls);
		cdev_del(&rc4ml_cdev);
		unregister_chrdev_region(rc4ml_devno, 1);
		return res;
	}

	rc4ml_device = device_create(cls, NULL, rc4ml_devno, NULL, "rc4ml_dev");
	if (IS_ERR(rc4ml_device))
	{
		res = PTR_ERR(rc4ml_device);
		class_destroy(cls);
		cdev_del(&rc4ml_cdev);
		unregister_chrdev_region(rc4ml_devno, 1);
		return res;
	}

	printk("rc4ml device registered: major=%d minor=%d\n", MAJOR(rc4ml_devno), MINOR(rc4ml_devno));
	printk("rc4ml:init complete\n");
	return 0;
}

void rc4ml_cleanup(void)
{
	device_destroy(cls, rc4ml_devno);
	class_destroy(cls);
	cdev_del(&rc4ml_cdev);
	unregister_chrdev_region(rc4ml_devno, 1);
	printk("rc4ml:destroy complete\n");
}
