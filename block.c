#include <linux/version.h>

#include "block.h"

static blk_qc_t vpciedisk_submit_bio(struct bio *bio) {
    struct graid_dev *dev = bio->bi_bdev->bd_disk->private_data;
    struct bio* child_bio, *tar_bio;
    unsigned int devi;
    sector_t sta_sector, end_sector, cnt_sectors;

bio_split:

    sta_sector = bio->bi_iter.bi_sector;
    end_sector = stripe_end_sector(sta_sector);

    if(end_sector + 1 >= bio_end_sector(bio)) {
        end_sector = bio_end_sector(bio) - 1;
        tar_bio = bio;
        goto bio_submit;
    }

    cnt_sectors = end_sector - sta_sector + 1;

    child_bio = bio_split(bio, cnt_sectors, GFP_NOIO, &dev->queue->bio_split);
    if(!child_bio) {
        GRAID_ERROR("split bio failed.\n");
        bio_io_error(bio);
        goto vp_submit_bio_out;
    }

    child_bio->bi_private = NULL;
    child_bio->bi_end_io = NULL;
    bio_chain(child_bio, bio);

    tar_bio = child_bio;

bio_submit:
    devi = device_num(stripe_num(sta_sector), dev->disk_cnt);

    tar_bio->bi_iter.bi_sector = sector_whole_to_i(sta_sector, dev->disk_cnt);
    bio_set_dev(tar_bio, dev->bdev[devi]);
    
    if(bio_data_dir(tar_bio) == WRITE) {
        pcievdrv_submit_verify(tar_bio, devi, dev);
    }

    // GRAID_INFO("sta_sector=%llu, end_sector=%llu, devi=%u", sta_sector, end_sector, devi);
    submit_bio(tar_bio);

    if(tar_bio != bio) {
        goto bio_split;
    }

vp_submit_bio_out:
    return BLK_QC_T_NONE;
}

static int vpciedisk_open(struct block_device *bdev, fmode_t mode) {
    // struct graid_dev *dev = bdev->bd_disk->private_data;
    GRAID_INFO("open vpciedisk device.");
    // spin_lock(&dev->blk_lock);
    return 0;
}

static void vpciedisk_release(struct gendisk *disk, fmode_t mode) {
    // struct graid_dev *dev = disk->private_data;
    GRAID_INFO("release vpciedisk device.");
    // spin_unlock(&dev->blk_lock);
}

static int vpciedisk_getgeo(struct block_device *bdev, struct hd_geometry *geo) {
    struct graid_dev *dev = bdev->bd_disk->private_data;

    geo->cylinders = dev->size >> 6; // 磁道数
    geo->heads = 4; // 磁头数
    geo->sectors = 16; // 每个磁道上的扇区数
    geo->start = 4; // 块设备的起始扇区

    GRAID_INFO("get vpciedisk device geometry.");

    return 0;
}

struct block_device_operations vpciedisk_dev_ops = {
    .owner = THIS_MODULE,
    .open = vpciedisk_open,
    .release = vpciedisk_release,
    .getgeo = vpciedisk_getgeo,
    .submit_bio = vpciedisk_submit_bio,
};

static int create_block_device(struct graid_dev *dev) {
    int err;
    uint64_t nr_sectors;

    dev->size = dev->config.size_nvme_disk * (uint64_t)dev->disk_cnt;

    nr_sectors = dev->size >> KERNEL_SECTOR_SHIFT;

    spin_lock_init(&dev->blk_lock);

    dev->gd = blk_alloc_disk(NUMA_NO_NODE);
    if(!dev->gd || IS_ERR(dev->gd->queue)) {
        GRAID_INFO("alloc disk failure\n");
        goto out_err;
    }

    dev->gd->major = VPCIEDISK_MAJOR;
    dev->gd->first_minor = 0;
    dev->gd->minors = 1;
    dev->gd->fops = &vpciedisk_dev_ops;
    dev->queue = dev->gd->queue;
    dev->gd->private_data = dev;
    snprintf(dev->gd->disk_name, 32, VPCIEDISK_NAME);
    set_capacity(dev->gd, nr_sectors * (HARDSECT_SIZE / KERNEL_SECTOR_SIZE));
    blk_queue_logical_block_size(dev->queue, KERNEL_SECTOR_SIZE);

    if((err = add_disk(dev->gd)) < 0) {
        GRAID_ERROR("add disk failure, error code %d \n", err);
        goto out_disk_init;
    }

    init_waitqueue_head(&graid_dev->verify_wait_queue);

    return 0;

out_disk_init:
    if(dev->gd) {
        del_gendisk(dev->gd);
        blk_cleanup_disk(dev->gd);
    }

out_err:
    return -ENOMEM;
}

static void delete_block_device(struct graid_dev *dev) {
    if(dev->gd) {
        del_gendisk(dev->gd);
        blk_cleanup_disk(dev->gd);
    }

    GRAID_INFO("deleted vpciedisk device.");
}

int vpciedisk_init(struct graid_dev *graid_dev) {
    int status;

    status = register_blkdev(VPCIEDISK_MAJOR, VPCIEDISK_NAME);
    if(status < 0) {
        GRAID_ERROR("unable to register vpciedisk device.\n");
        return -EBUSY;
    }

    status = create_block_device(graid_dev);
    if(status < 0) {
        GRAID_ERROR("unable to create vpciedisk device.\n");
        goto out_register;
    }

    return 0;

out_register:
    unregister_blkdev(VPCIEDISK_MAJOR, VPCIEDISK_NAME);
    return -ENOMEM;
}

void vpciedisk_exit(struct graid_dev *graid_dev) {
    delete_block_device(graid_dev);
    unregister_blkdev(VPCIEDISK_MAJOR, VPCIEDISK_NAME);
}