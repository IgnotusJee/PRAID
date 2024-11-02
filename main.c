#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>

#ifdef CONFIG_X86
#include <asm/e820/types.h>
#include <asm/e820/api.h>
#endif

#include "nraid.h"
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

struct nraid_dev *nraid_dev = NULL;

unsigned long memmap_start = 0;
unsigned long memmap_size = 0;
unsigned int cpu = 0;

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
		NRAID_ERROR("[mem %#010lx-%#010lx] is usable, not reseved region\n",
			    (unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}

	if (!e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RESERVED)) {
		NRAID_ERROR("[mem %#010lx-%#010lx] is not reseved region\n",
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
		NRAID_ERROR("[memmap_start] should be specified\n");
		return -EINVAL;
	}

	if (!memmap_size) {
		NRAID_ERROR("[memmap_size] should be specified\n");
		return -EINVAL;
	} else if (memmap_size <= MB(1)) {
		NRAID_ERROR("[memmap_size] should be bigger than 1 MiB\n");
		return -EINVAL;
	}

	if (!cpu) {
		NRAID_ERROR("[cpu] shoud be spcified\n");
		return -EINVAL;
	}

	if (__validate_configs_arch()) {
		return -EPERM;
	}

	if (!major) {
		NRAID_ERROR("[major] should be specified\n");
		return -EINVAL;
	}

	if (per_size < MB(512)) {
		NRAID_ERROR("Disk size too small\n");
		return -EINVAL;
	}

	return 0;
}

static bool __load_configs(struct nraid_config *config) {
    bool first = true;
	unsigned int minor_nr;
	char *minor;

	if (__validate_configs() < 0) {
		return false;
	}

    config->nvme_major = major;
    config->size_nvme_disk = per_size;

	config->nr_nvme_disks = 0;

	while ((minor = strsep(&minors, ",")) != NULL) {
		minor_nr = (unsigned int)simple_strtol(minor, NULL, 10);
		if (first) {
			config->nvme_minor_verify = minor_nr;
		} else {
			config->nvme_minor[config->nr_nvme_disks] = minor_nr;
			config->nr_nvme_disks++;
		}
		first = false;

		if(config->nr_nvme_disks > 31) {
			NRAID_ERROR("To many disks.\n");
			return false;
		}
	}

	if((unsigned long)(config->nr_nvme_disks + 1) * CHUNK_SIZE + PAGE_SIZE > memmap_size) {
		NRAID_ERROR("Spcace proviced too small.\n");
		return false;
	}

	return true;
}

static int nvme_blkdev_init(struct nraid_dev *dev) {
    int i;

    dev->disk_cnt = dev->config.nr_nvme_disks;
    
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

static void nvme_blkdev_final(struct nraid_dev *dev) {
    int i;

    
    blkdev_put(dev->bdev_verify, FMODE_READ | FMODE_WRITE);
    for(i = 0; i < dev->disk_cnt; i ++) {
        blkdev_put(dev->bdev[i], FMODE_READ | FMODE_WRITE);
    }
}

static void __print_nraid_info(struct nraid_dev *dev) {
	NRAID_INFO("size_nvme_disk = %lld\n", dev->config.size_nvme_disk);
	NRAID_INFO("disk size = %lld\n", dev->size);
	NRAID_INFO("disk count = %d\n", dev->disk_cnt);
}

static int vpcie_module_init(void) {
    int ret = 0;

	nraid_dev = kzalloc(sizeof(struct nraid_dev), GFP_KERNEL);

	if(!nraid_dev) {
		ret = -EINVAL;
		goto out_err;
	}

    if (!__load_configs(&nraid_dev->config)) {
        ret = -EINVAL;
		goto out_pcievdrv_err;
	}

	if(nvme_blkdev_init(nraid_dev) < 0) {
		ret = -EBUSY;
		goto out_pcievdrv_err;
	}

	if(PCIEV_init(nraid_dev->bdev_verify, nraid_dev->disk_cnt) < 0) {
		ret = -EBUSY;
		goto out_nvme_err;
	}

    ret = pcievdrv_init();
    if(ret) {
        goto out_device_err;
    }

    ret = vpciedisk_init(nraid_dev);
    if(ret) {
        goto out_vdisk_err;
    }

	__print_nraid_info(nraid_dev);

    return 0;

out_vdisk_err:
    pcievdrv_exit();

out_device_err:
	PCIEV_exit();

out_nvme_err:
	nvme_blkdev_final(nraid_dev);

out_pcievdrv_err:
	if(nraid_dev) {
		kfree(nraid_dev);
	}

out_err:
    return ret;
}

static void vpcie_module_exit(void) {
    vpciedisk_exit(nraid_dev);
    pcievdrv_exit();
	PCIEV_exit();
	nvme_blkdev_final(nraid_dev);
	if(nraid_dev) {
		kfree(nraid_dev);
	}
}

MODULE_LICENSE("GPL v2");
module_init(vpcie_module_init);
module_exit(vpcie_module_exit);