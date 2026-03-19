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
#include <asm/io.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/page_types.h>
#include "rc4ml.h"
#include <linux/version.h>   // LINUX_VERSION_CODE, KERNEL_VERSION
#include <linux/pagemap.h>   
#include <linux/cdev.h>

static struct cdev rc4ml_cdev;

MODULE_LICENSE("GPL");

static dev_t rc4ml_devno;
static struct class *cls;
static struct device *rc4ml_device;
struct huge_table_t huge_table;
static int a;
size_t bridge_phy_addr;

static int rc4ml_open(struct inode *inode, struct file *pfile)
{
	printk("rc4ml dev opened\n");
	return 0;
}

static int rc4ml_release(struct inode *inode, struct file *pfile)
{
	printk("rc4ml dev released\n");
	return 0;
}

static int ioctl_huge_set(unsigned long arg)
{
	int rc;
	int i = 0;
	int j = 0;
	struct huge_mem buf;
	unsigned long npages;
	unsigned long paddr;
	unsigned long last_paddr = 0;

	rc = copy_from_user(&buf, (struct huge_mem *)arg, sizeof(struct huge_mem));
	if (rc != 0)
	{
		return rc;
	}
	npages = 1 + (buf.size - 1) / PAGE_SIZE;
	printk(KERN_INFO "req npages %ld\n", npages);
	printk(KERN_INFO "huge addr %px, size %ld\n", (void *)buf.vaddr, buf.size);
	huge_table.huge_pages = vmalloc(npages * sizeof(struct page *));

#ifndef MMAP_LOCK_INITIALIZER
	down_read(&current->mm->mmap_sem);
#else
	mmap_read_lock(current->mm);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
        rc = pin_user_pages_fast(buf.vaddr, npages, FOLL_WRITE, huge_table.huge_pages);
#else
        rc = get_user_pages(buf.vaddr, npages, FOLL_WRITE, huge_table.huge_pages, NULL);
#endif
	//rc = get_user_pages(buf.vaddr, npages, 1, huge_table.huge_pages, NULL);
#ifndef MMAP_LOCK_INITIALIZER
	up_read(&current->mm->mmap_sem);
#else
	mmap_read_unlock(current->mm);
#endif
	if (rc == 0)
	{
		printk(KERN_INFO "get_user_pages failed\n");
		return -1;
	}

	npages = rc;
	printk(KERN_INFO "recv npages %ld\n", npages);
	for (i = 0; i < npages; i++)
	{
		SetPageReserved(huge_table.huge_pages[i]);
		paddr = page_to_phys(huge_table.huge_pages[i]);
		if (paddr - last_paddr != 4096)
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
	huge_table.size = npages * 4096;
	huge_table.nhpages = j;
	huge_table.vaddr_start = buf.vaddr;

	printk(KERN_INFO "IOCTL_BUFFER_SET\n");
	return 0;
}

static int ioctl_huge_get(unsigned long arg)
{
	int rc;
	int i = 0;
	struct huge_mapping map;
	unsigned long *phy_addr;
	rc = copy_from_user(&map, (struct huge_mapping *)arg, sizeof(struct huge_mapping));
	if (rc != 0)
	{
		return rc;
	}
	printk(KERN_INFO "map huge page number:%ld\n", map.nhpages);
	map.nhpages = huge_table.nhpages;
	phy_addr = kmalloc(sizeof(unsigned long *) * map.nhpages, GFP_KERNEL);
	for (i = 0; i < map.nhpages; i++)
	{
		phy_addr[i] = page_to_phys(huge_table.huge_pages[i * 512]);
	}
	if (copy_to_user((struct huge_mapping *)arg, &map, sizeof(struct huge_mapping)))
	{
		goto error;
	}
	if (copy_to_user(map.phy_addr, phy_addr, sizeof(unsigned long *) * map.nhpages))
	{
		goto error;
	}

	printk(KERN_INFO "IOCTL_BUFFER_GET\n");
	kfree(phy_addr);
	return 0;
error:
	kfree(phy_addr);
	return -EACCES;
}

static long rc4ml_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	unsigned long rc = 0;
	printk("ioctl called.\n");
	switch (cmd)
	{
	case QUERY_GET_VALUE:
		rc = copy_to_user((int *)arg, &a, sizeof(int));
		break;
	case QUERY_CLEAR_VALUE:
		a = 0;
		rc = 0;
		break;
	case QUERY_SET_VALUE:
		rc = copy_from_user(&a, (int *)arg, sizeof(int));
		break;
	case HUGE_MAPPING_SET:
		ioctl_huge_set(arg);
		break;
	case HUGE_MAPPING_GET:
		ioctl_huge_get(arg);
		break;
	default:
		printk(KERN_ERR "Unknown ioctl command: %u\n", cmd);
		return -EINVAL;
	}
	return 0;
}

static struct file_operations rc4ml_ops = {
	.open = rc4ml_open,
	.release = rc4ml_release,
	.unlocked_ioctl = rc4ml_ioctl,
};

int rc4ml_init(void)
{
        int res;

        /* 1) 动态分配 major/minor */
        res = alloc_chrdev_region(&rc4ml_devno, rc4ml_minor, 1, "rc4ml_dev");
        if (res < 0) {
                pr_err("rc4ml: alloc_chrdev_region failed: %d\n", res);
                return res;
        }

        /* 2) 注册 cdev */
        cdev_init(&rc4ml_cdev, &rc4ml_ops);
        rc4ml_cdev.owner = THIS_MODULE;

        res = cdev_add(&rc4ml_cdev, rc4ml_devno, 1);
        if (res < 0) {
                pr_err("rc4ml: cdev_add failed: %d\n", res);
                unregister_chrdev_region(rc4ml_devno, 1);
                return res;
        }

        /* 3) 创建 class */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
        cls = class_create("rc4ml");
#else
        cls = class_create(THIS_MODULE, "rc4ml");
#endif
        if (IS_ERR(cls)) {
                res = PTR_ERR(cls);
                pr_err("rc4ml: class_create failed: %d\n", res);
                cdev_del(&rc4ml_cdev);
                unregister_chrdev_region(rc4ml_devno, 1);
                return res;
        }

        /* 4) 创建 /dev/rc4ml_dev */
        rc4ml_device = device_create(cls, NULL, rc4ml_devno, NULL, "rc4ml_dev");
        if (IS_ERR(rc4ml_device)) {
                res = PTR_ERR(rc4ml_device);
                pr_err("rc4ml: device_create failed: %d\n", res);
                class_destroy(cls);
                cdev_del(&rc4ml_cdev);
                unregister_chrdev_region(rc4ml_devno, 1);
                return res;
        }

        pr_info("rc4ml: init complete (major=%d minor=%d)\n",
                MAJOR(rc4ml_devno), MINOR(rc4ml_devno));
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
