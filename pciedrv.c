#include <linux/pci.h>
#include <linux/module.h>
#include <linux/bio.h>

#include "graid.h"
#include "pciev.h"
#include "pciedrv.h"
#include "block.h"

/* 指明该驱动程序适用于哪一些PCI设备 */
static struct pci_device_id pcievdrv_ids[] = {
    {PCI_DEVICE(PCIEV_VENDOR_ID, PCIEV_DEVICE_ID)},
    {0, },
};
MODULE_DEVICE_TABLE(pci, pcievdrv_ids);

static void pcievdrv_get_configs(struct pci_dev *dev) {
    uint8_t val1;
    uint16_t val2;
    uint32_t val4;
    pci_read_config_word(dev, PCI_VENDOR_ID, &val2);
    VP_INFO("vendorID: %x\n", val2);
    pci_read_config_word(dev, PCI_DEVICE_ID, &val2);
    VP_INFO("deviceID: %x\n", val2);
    pci_read_config_byte(dev,  PCI_REVISION_ID, &val1);
    VP_INFO("revisionID: %x\n", val1);
    pci_read_config_dword(dev, PCI_CLASS_REVISION, &val4);
    VP_INFO("class: %x\n", val4);
}

static void verify_task_end_io(struct bio *bio) {
    struct pciev_bio_private *data;

    data = bio->bi_private;
    if(!data) {
        VP_ERROR("bio no private data\n");
        return;
    }
    
    data->done_cnt ++;
    if(data->done_cnt < 2) {
        return;
    }

    if(!copy_page_to_buffer(data->page_old, PTR_BAR_TO_STRIPE_O(data->dev->stripe_addr), data->offset, data->size)
    ||!copy_page_to_buffer(data->page_new, PTR_BAR_TO_STRIPE_N(data->dev->stripe_addr), data->offset, data->size)
    ||!copy_page_to_buffer(data->page_verify, PTR_BAR_TO_STRIPE_V(data->dev->stripe_addr), data->offset, data->size)) {
        VP_ERROR("copy stripe failed.\n");
        data->dev->bar->io_property.db = DB_FREE;
    }
    else {
        data->dev->bar->io_property.db = DB_BUSY;
        // data->dev->bar->io_property.db = DB_DONE;
    }

    bio_put(bio);
}

static bool read_stripe_async(struct page *page, struct block_device *bdev, sector_t num_sector, uint64_t offset, uint64_t size, struct graid_dev *dev, struct pciev_bio_private *data) {
    struct bio *bio;

    bio = bio_alloc(GFP_KERNEL, 1);
    if(!bio) {
        VP_ERROR("bio alloc failed.\n");
        goto out_fail;
    }

    bio_set_dev(bio, bdev);
    bio->bi_iter.bi_sector = num_sector;
    bio->bi_end_io = verify_task_end_io;
    bio->bi_private = data;

    if(bio_add_page(bio, page, size, offset) != size) {
        VP_ERROR("bio add page failed.\n");
        goto out_bio;
    }

    if(bdev == graid_dev->bdev_verify) {
        // VP_INFO("sta_sector=%llu, size=%lu, offset=%lu", num_sector, size, offset);
    }

    bio_set_op_attrs(bio, REQ_OP_READ, 0);
    submit_bio(bio);

    return true;

out_bio:
    bio_put(bio);
out_fail:
    return false;
}

static bool read_stripe_sync(struct page *page, struct block_device *bdev, sector_t num_sector, uint64_t offset, uint64_t size, struct graid_dev *dev, struct pciev_bio_private *data) {
    struct bio *bio;

    if(!data) {
        VP_ERROR("bio no private data\n");
        goto out_fail;
    }

    bio = bio_alloc(GFP_KERNEL, 1);
    if(!bio) {
        VP_ERROR("bio alloc failed.\n");
        goto out_fail;
    }

    bio_set_dev(bio, bdev);
    bio->bi_iter.bi_sector = num_sector;

    if(bio_add_page(bio, page, size, offset) != size) {
        VP_ERROR("bio add page failed.\n");
        goto out_bio;
    }

    if(bdev == graid_dev->bdev_verify) {
        // VP_INFO("sta_sector=%llu, size=%lu, offset=%lu", num_sector, size, offset);
    }

    bio_set_op_attrs(bio, REQ_OP_READ, 0);
    submit_bio_wait(bio);

    data->done_cnt ++;

    if(data->done_cnt == 2) {
        if(!copy_page_to_buffer(data->page_old, PTR_BAR_TO_STRIPE_O(data->dev->stripe_addr), data->offset, data->size)
        ||!copy_page_to_buffer(data->page_new, PTR_BAR_TO_STRIPE_N(data->dev->stripe_addr), data->offset, data->size)
        ||!copy_page_to_buffer(data->page_verify, PTR_BAR_TO_STRIPE_V(data->dev->stripe_addr), data->offset, data->size)) {
            VP_ERROR("copy stripe failed.\n");
            data->dev->bar->io_property.db = DB_FREE;
        }
        else {
            data->dev->bar->io_property.db = DB_BUSY;
        }
    }

    return true;

out_bio:
    bio_put(bio);
out_fail:
    return false;
}

static bool do_verify_task(struct page *page_new, sector_t num_sector, unsigned int devi, uint64_t offset, uint64_t size, struct graid_dev *dev) {
    dev->bar->io_property.db = DB_WRITE;

    VP_INFO("size=%llu, offset=%llu, num_sector=%llu\n", size, offset, num_sector);

    dev->data_private->size = size;
    dev->data_private->offset = offset;
    dev->data_private->dev = dev;
    dev->data_private->done_cnt = 0;

    dev->bar->io_property.sector_sta = num_sector;
    dev->bar->io_property.offset = offset;
    dev->bar->io_property.size = size;

    copy_page_to_page(dev->data_private->page_new, page_new);

    if(!read_stripe_async(dev->data_private->page_old, dev->bdev[devi], num_sector, offset, size, dev, dev->data_private)
    || !read_stripe_async(dev->data_private->page_verify, dev->bdev_verify, num_sector, offset, size, dev, dev->data_private)) {
        VP_ERROR("read stripe failed.\n");
        goto out;
    }

    return true;

out:
    dev->bar->io_property.db = DB_FREE;
    return false;
}

bool pcievdrv_submit_verify(struct bio *bio, unsigned int devi, struct graid_dev *dev) {
    struct bio_vec bvec;
	struct bvec_iter iter;
    bool ret;
    sector_t pos_sector = bio->bi_iter.bi_sector;

    bio_for_each_segment(bvec, bio, iter) {
        if(dev->bar->io_property.db != DB_FREE) {
            wait_event_interruptible(dev->verify_wait_queue, dev->bar->io_property.db == DB_FREE);
        }
        ret = do_verify_task(bvec.bv_page, sector_whole_to_i(pos_sector, dev->disk_cnt), devi, bvec.bv_offset, bvec.bv_len, dev);
        pos_sector += bvec.bv_len >> KERNEL_SECTOR_SHIFT;
        if(!ret) {
            return false;
        }
    }

    return true;
}

/* 设备中断服务 */
static irqreturn_t pcievdrv_interrupt(int irq, void *dev_id) {
    struct graid_dev *graid_dev = (struct graid_dev *)dev_id;
    
    if(graid_dev->bar->io_property.db != DB_DONE) {
        return IRQ_NONE;
    }

    graid_dev->bar->io_property.db = DB_FREE;
    wake_up_interruptible(&graid_dev->verify_wait_queue);

    return IRQ_HANDLED;
}

/* 需设置bar寄存器，但因为pcievirt已经设置了这里不用 */
static int pcievdrv_probe(struct pci_dev *dev, const struct pci_device_id *id) {
    int ret = 0;
    resource_size_t stripe_sta;
    size_t stripe_range;

    VP_INFO("probing function\n");

    if(pci_enable_device(dev)) {
        VP_ERROR("cannot enable device.\n");
        ret = -EIO;
        goto out_final;
    }

    graid_dev->irq = dev->irq;
    if(graid_dev->irq < 0) {
        VP_ERROR("invalid irq %d\n", graid_dev->irq);
        ret = -EINVAL;
        goto out_final;
    }

    graid_dev->mem_sta = pci_resource_start(dev, 0);
    graid_dev->range = pci_resource_end(dev, 0) - graid_dev->mem_sta + 1;
    VP_INFO("start %llx %lx\n", graid_dev->mem_sta, graid_dev->range);

    ret = pci_request_regions(dev, PCIEVIRT_DRV_NAME);
    if(ret) {
        VP_ERROR("PCI request regions err\n");
        ret = -EINVAL;
        goto out_final;
    }

    graid_dev->data_private = kzalloc(sizeof(struct pciev_bio_private), GFP_KERNEL);

    if(!graid_dev->data_private) {
        VP_INFO("alloc private data failed.\n");
        ret = -ENOSPC;
        goto out_regions;
    }

    graid_dev->data_private->dev = graid_dev;
    graid_dev->data_private->page_new = alloc_page(GFP_KERNEL);
    graid_dev->data_private->page_old = alloc_page(GFP_KERNEL);
    graid_dev->data_private->page_verify = alloc_page(GFP_KERNEL);

    if(
        !graid_dev->data_private->page_new ||
        !graid_dev->data_private->page_old ||
        !graid_dev->data_private->page_verify
    ) {
        VP_INFO("alloc pages failed.\n");
        ret = -ENOSPC;
        goto out_data;
    }

    /* 将mem资源映射到虚拟地址 */
    graid_dev->bar = memremap(graid_dev->mem_sta, PAGE_SIZE, MEMREMAP_WT);

    if(!graid_dev->bar) {
        VP_ERROR("bar memremap err.\n");
        ret = -ENOMEM;
        goto out_pages;
    }

    VP_INFO("bar memremap in: 0x%p\n", graid_dev->bar);

    stripe_sta = graid_dev->mem_sta + BAR_STRIPE_OFFSET;
    stripe_range = 3 * STRIPE_SIZE;

    // if(graid_dev->range < stripe_range + BAR_STRIPE_OFFSET) {
    //     VP_ERROR("request size larger than provided.\n");
    //     ret = -ENOMEM;
    //     goto out_regions;
    // }

    graid_dev->stripe_addr = memremap(stripe_sta, stripe_range, MEMREMAP_WB);

    if(!graid_dev->stripe_addr) {
        VP_ERROR("storage memremap err.\n");
        ret = -ENOMEM;
        goto out_memunmap_bar;
    }

    /* 申请中断IRQ并设定中断服务子函数 */
    ret = request_irq(graid_dev->irq, pcievdrv_interrupt, IRQF_SHARED, PCIEVIRT_DRV_NAME, graid_dev);
    if(ret) {
        VP_ERROR(KERN_ERR "Can't get assigned IRQ %d.\n", graid_dev->irq);
        goto out_memunmap_sto;
    }

    pci_set_drvdata(dev, graid_dev);
    VP_INFO("Probe succeeds.PCIE memory addr start at %llX, mypci->bar is 0x%p,interrupt No. %d.\n", graid_dev->mem_sta, graid_dev->bar, graid_dev->irq);
    pcievdrv_get_configs(dev);

    return 0;

out_memunmap_sto:
    memunmap(graid_dev->stripe_addr);
out_memunmap_bar:
    memunmap(graid_dev->bar);
out_pages:
    if(graid_dev->data_private->page_new) {
        __free_page(graid_dev->data_private->page_new);
    }
    if(graid_dev->data_private->page_old) {
        __free_page(graid_dev->data_private->page_old);
    }
    if(graid_dev->data_private->page_verify) {
        __free_page(graid_dev->data_private->page_verify);
    }
out_data:
    if(graid_dev->data_private) {
        kfree(graid_dev->data_private);
    }
out_regions:
    pci_release_regions(dev);
out_final:
    return ret;
}

static void pcievdrv_remove(struct pci_dev *dev) {
    struct graid_dev *graid_dev = pci_get_drvdata(dev);
    free_irq(graid_dev->irq, graid_dev);
    memunmap(graid_dev->stripe_addr);
    memunmap(graid_dev->bar);
    pci_release_regions(dev);
    pci_disable_device(dev);
    VP_INFO("driver removed.\n");
}

static struct pci_driver pcievdrv_driver = {
    .name = PCIEVIRT_DRV_NAME,
    .id_table = pcievdrv_ids,
    .probe = pcievdrv_probe,
    .remove = pcievdrv_remove
};

int pcievdrv_init(void) {
    VP_INFO("init pcivirt driver.\n");
    return pci_register_driver(&pcievdrv_driver);
}

void pcievdrv_exit(void) {
    VP_INFO("exit pcievdrv driver.\n");
    pci_unregister_driver(&pcievdrv_driver);
}