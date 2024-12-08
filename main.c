#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>

#ifdef CONFIG_X86
#include <asm/e820/types.h>
#include <asm/e820/api.h>
#endif

#include "praid.h"
#include "pciedrv.h"
#include "block.h"
#include "device.h"
#include "pciev.h"

#define MODULE_NAME "PCIEDRVBLK"

/**
 * Memory Layout
 *
 * 1MiB area for metadata
 *  - BAR : 1 page
 * Storage area
*/

struct praid_dev *praid_dev = NULL;

static unsigned long memmap_start = 0;
static unsigned long memmap_size = 0;
static unsigned int cpu = 0;

static unsigned int major = 0;
static uint64_t per_size = 0;
static char *minors;

static int set_parse_mem_param(const char *val, const struct kernel_param *kp) {
	uint64_t *arg = (uint64_t *)kp->arg;
	*arg = memparse(val, NULL);
	return 0;
}

static struct kernel_param_ops ops_parse_mem_param = {
	.set = set_parse_mem_param,
	.get = param_get_ulong,
};

module_param_cb(memmap_start, &ops_parse_mem_param, &memmap_start, 0444);
MODULE_PARM_DESC(memmap_start, "Reserved memory address");
module_param_cb(memmap_size, &ops_parse_mem_param, &memmap_size, 0444);
MODULE_PARM_DESC(memmap_size, "Reserved memory size");
module_param(cpu, uint, 0644);
MODULE_PARM_DESC(cpu, "CPU core to do dispatcher jobs");
module_param_cb(per_size, &ops_parse_mem_param, &per_size, 0444);
MODULE_PARM_DESC(per_size, "Storage size for single nvme device");
module_param(major, uint, 0644);
MODULE_PARM_DESC(major, "Major device number of nvme block device");
module_param(minors, charp, 0644);
MODULE_PARM_DESC(minors, "Minor device number of nvme block devices");

#ifdef CONFIG_X86
static int __validate_configs_arch(void) {
	unsigned long resv_start_bytes;
	unsigned long resv_end_bytes;

	resv_start_bytes = memmap_start;
	resv_end_bytes = resv_start_bytes + memmap_size - 1;

	if (e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RAM) ||
	    e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RESERVED_KERN)) {
		PRAID_ERROR("[mem %#010lx-%#010lx] is usable, not reseved region\n",
			    (unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}

	if (!e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RESERVED)) {
		PRAID_ERROR("[mem %#010lx-%#010lx] is not reseved region\n",
			    (unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}
	return 0;
}
#else
static int __validate_configs_arch(void) {
	return -EPERM;
}
#endif

static int __validate_configs(void) {
	if (!memmap_start) {
		PRAID_ERROR("[memmap_start] should be specified\n");
		return -EINVAL;
	}

	if (!memmap_size) {
		PRAID_ERROR("[memmap_size] should be specified\n");
		return -EINVAL;
	} else if (memmap_size <= MB(1)) {
		PRAID_ERROR("[memmap_size] should be bigger than 1 MiB\n");
		return -EINVAL;
	}

	if (!cpu) {
		PRAID_ERROR("[cpu] shoud be spcified\n");
		return -EINVAL;
	}

	if (__validate_configs_arch()) {
		return -EPERM;
	}

	if (!major) {
		PRAID_ERROR("[major] should be specified\n");
		return -EINVAL;
	}

	if (per_size < MB(512)) {
		PRAID_ERROR("Disk size too small\n");
		return -EINVAL;
	}

	return 0;
}

static bool __load_configs(struct praid_config *config) {
    bool first = true;
	unsigned int minor_nr;
	char *minor;

	if (__validate_configs() < 0) {
		return false;
	}

    config->nvme_major = major;
    config->size_nvme_disk = per_size;

	config->nr_data_disks = 0;

	while ((minor = strsep(&minors, ",")) != NULL) {
		minor_nr = (unsigned int)simple_strtol(minor, NULL, 10);
		if (first) {
			config->nvme_minor_verify = minor_nr;
			first = false;
		} else {
			config->nvme_minor[config->nr_data_disks] = minor_nr;
			config->nr_data_disks ++;
		}

		if(config->nr_data_disks > 31) {
			PRAID_ERROR("To many disks.\n");
			return false;
		}
	}

	if((unsigned long)(((config->nr_data_disks * config->size_nvme_disk) >> (CHUNK_SHIFT - 2)) + STORAGE_OFFSET) > memmap_size) {
		PRAID_ERROR("Spcace proviced too small.\n");
		return false;
	}

	config->memmap_start = memmap_start;
	config->memmap_size = memmap_size;
	config->storage_start = memmap_start + STORAGE_OFFSET;
	config->storage_size = memmap_size - STORAGE_OFFSET;
	config->cpu_nr_dispatcher = cpu;

	return true;
}

static int nvme_blkdev_init(struct praid_dev *dev) {
    int i;

    dev->disk_cnt = dev->config.nr_data_disks;
    dev->size = dev->config.size_nvme_disk * (uint64_t)dev->disk_cnt;
	dev->nr_stripe = dev->size >> CHUNK_SHIFT;
    
    for(i = 0; i < dev->disk_cnt; i ++) {
        if(IS_ERR((dev->bdev[i] = blkdev_get_by_dev(
        MKDEV(dev->config.nvme_major, dev->config.nvme_minor[i]),
        FMODE_READ | FMODE_WRITE,
        NULL)))) {
            goto ret_err;
        }
    }

    if(IS_ERR((dev->bdev_verify = blkdev_get_by_dev(
    MKDEV(dev->config.nvme_major, dev->config.nvme_minor_verify),
    FMODE_READ | FMODE_WRITE,
    NULL)))) {
        goto ret_err;
    }

    return 0;

ret_err:
    for(i --; i >= 0; i --) {
        blkdev_put(dev->bdev[i], FMODE_READ | FMODE_WRITE); 
    }

    return -EBUSY;
}

static void nvme_blkdev_final(struct praid_dev *dev) {
    int i;

    
    blkdev_put(dev->bdev_verify, FMODE_READ | FMODE_WRITE);
    for(i = 0; i < dev->disk_cnt; i ++) {
        blkdev_put(dev->bdev[i], FMODE_READ | FMODE_WRITE);
    }
}

static void __print_praid_info(struct praid_dev *dev) {
	PRAID_INFO("size_nvme_disk = %lld\n", dev->config.size_nvme_disk);
	PRAID_INFO("disk size = %lld\n", dev->size);
	PRAID_INFO("disk count = %d\n", dev->disk_cnt);
}

static int vpcie_module_init(void) {
    int ret = 0;

	praid_dev = kzalloc(sizeof(struct praid_dev), GFP_KERNEL);

	if(!praid_dev) {
		ret = -EINVAL;
		goto out_err;
	}

    if (!__load_configs(&praid_dev->config)) {
        ret = -EINVAL;
		goto out_pcievdrv_err;
	}

	if(nvme_blkdev_init(praid_dev) < 0) {
		ret = -EBUSY;
		goto out_pcievdrv_err;
	}

	if(PCIEV_init(praid_dev) < 0) {
		ret = -EBUSY;
		goto out_nvme_err;
	}

    ret = pcievdrv_init();
    if(ret) {
        goto out_device_err;
    }

    ret = vpciedisk_init(praid_dev);
    if(ret) {
        goto out_vdisk_err;
    }

	__print_praid_info(praid_dev);

    return 0;

out_vdisk_err:
    pcievdrv_exit();

out_device_err:
	PCIEV_exit();

out_nvme_err:
	nvme_blkdev_final(praid_dev);

out_pcievdrv_err:
	if(praid_dev) {
		kfree(praid_dev);
	}

out_err:
    return ret;
}

static void vpcie_module_exit(void) {
    vpciedisk_exit(praid_dev);
    pcievdrv_exit();
	PCIEV_exit();
	nvme_blkdev_final(praid_dev);
	if(praid_dev) {
		kfree(praid_dev);
	}
}

MODULE_LICENSE("GPL v2");
module_init(vpcie_module_init);
module_exit(vpcie_module_exit);