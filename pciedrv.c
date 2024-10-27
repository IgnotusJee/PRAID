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

static bool read_stripe(struct page *page, struct block_device *bdev, struct verify_work_param* param) {
    struct bio *bio;

    bio = bio_alloc(GFP_KERNEL, 1);
    if(!bio) {
        VP_ERROR("bio alloc failed.\n");
        goto out_fail;
    }

    bio_set_dev(bio, bdev);
    bio->bi_iter.bi_sector = param->num_sector;

    VP_INFO("Addbio: size=%llu, offset=%llu, num_sector=%llu\n", param->size, param->offset, param->num_sector);

    if(bio_add_page(bio, page, param->size, param->offset) != param->size) {
        VP_ERROR("bio add page failed.\n");
        goto out_bio;
    }

    bio_set_op_attrs(bio, REQ_OP_READ, 0);
    submit_bio_wait(bio);

    param->done_cnt ++;

    VP_DEBUG("cnt=%d\n", param->done_cnt);
    if(param->done_cnt == 2) {
        if(down_interruptible(&param->dev->sem)) {
            VP_ERROR("Wait interrupted");
            return;
        }

        param->dev->bar->io_property.sector_sta = param->num_sector;
        param->dev->bar->io_property.offset = param->offset;
        param->dev->bar->io_property.size = param->size;

        if(!copy_page_to_buffer(param->page_old, PTR_BAR_TO_STRIPE_O(param->dev->stripe_addr), param->offset, param->size)
        ||!copy_page_to_buffer(param->page_new, PTR_BAR_TO_STRIPE_N(param->dev->stripe_addr), param->offset, param->size)
        ||!copy_page_to_buffer(param->page_verify, PTR_BAR_TO_STRIPE_V(param->dev->stripe_addr), param->offset, param->size)) {
            VP_ERROR("copy stripe failed.\n");
            goto out_sem;
        }
        else {
            param->dev->bar->io_property.io_num ++;
        }
    }

    bio_put(bio);
    return true;

out_sem:
    up(&param->dev->sem);
out_bio:
    bio_put(bio);
out_fail:
    return false;
}

static void do_verify_work(struct work_struct *work) {
    struct verify_work* work_data = container_of(work, struct verify_work, work);
    struct verify_work_param* param = &work_data->param;

    VP_DEBUG("size=%llu, offset=%llu, num_sector=%llu\n", param->size, param->offset, param->num_sector);

    param->page_old = alloc_page(GFP_KERNEL);
    param->page_verify = alloc_page(GFP_KERNEL);
    if(!param->page_old || !param->page_verify) {
        VP_ERROR("Alloc page failed.\n");
        goto out;
    }

    if(!read_stripe(param->page_old, param->dev->bdev[param->devi], param)
    || !read_stripe(param->page_verify, param->dev->bdev_verify, param)) {
        VP_ERROR("read stripe error.\n");
    }

out:
    if(param->page_old) {
        __free_page(param->page_old);
    }
    if(param->page_verify) {
        __free_page(param->page_verify);
    }
    if(param->page_new) {
        __free_page(param->page_new);
    }
    kfree(work_data);
}

static bool add_verify_task(struct page *page_new, sector_t num_sector, unsigned int devi, uint64_t offset, uint64_t size, struct graid_dev *dev) {
    struct verify_work* work = kmalloc(sizeof(struct verify_work), GFP_KERNEL);

    if(!work) {
        VP_ERROR("Alloc work struct failed.\n");
        return false;
    }

    work->param.page_new = alloc_page(GFP_KERNEL);
    if(!work->param.page_new) {
        VP_ERROR("Alloc new page failed.\n");
        kfree(work);
        return false;
    }

    copy_page_to_page(work->param.page_new, page_new);

    work->param.done_cnt = 0;
    work->param.num_sector = num_sector;
    work->param.devi = devi;
    work->param.offset = offset;
    work->param.size = size;
    work->param.dev = dev;
    INIT_WORK(&work->work, do_verify_work);
    
    if (!queue_work(dev->workqueue, &work->work)) {
        VP_ERROR("Add work failed.\n");
        if(work->param.page_new) {
            __free_page(work->param.page_new);
        }
        kfree(work);
        return false;
    }

    return true;
}

bool pcievdrv_submit_verify(struct bio *bio, unsigned int devi, struct graid_dev *dev) {
    struct bio_vec bvec;
	struct bvec_iter iter;
    bool ret;
    sector_t pos_sector = bio->bi_iter.bi_sector;

    VP_DEBUG("outter, devi=%u\n", devi);
    bio_for_each_segment(bvec, bio, iter) {
        VP_DEBUG("inner, devi=%u\n", devi);
        ret = add_verify_task(bvec.bv_page, sector_whole_to_i(pos_sector, dev->disk_cnt), devi, bvec.bv_offset, bvec.bv_len, dev);
        pos_sector += bvec.bv_len >> KERNEL_SECTOR_SHIFT;
        if(!ret) {
            return false;
        }
    }

    return true;
}

static irqreturn_t pcievdrv_interrupt(int irq, void *dev_id) {
    struct graid_dev *graid_dev = (struct graid_dev *)dev_id;
    VP_DEBUG("");
    up(&graid_dev->sem);

    return IRQ_HANDLED;
}

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

    /* 将mem资源映射到虚拟地址 */
    graid_dev->bar = memremap(graid_dev->mem_sta, PAGE_SIZE, MEMREMAP_WT);

    if(!graid_dev->bar) {
        VP_ERROR("bar memremap err.\n");
        ret = -ENOMEM;
        goto out_regions;
    }

    VP_INFO("bar memremap in: 0x%p\n", graid_dev->bar);

    stripe_sta = graid_dev->mem_sta + BAR_STRIPE_OFFSET;
    stripe_range = 3 * STRIPE_SIZE;

    sema_init(&graid_dev->sem, 1);
    graid_dev->workqueue = create_workqueue("verfy_task_work_queue");

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
    destroy_workqueue(graid_dev->workqueue);
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
    flush_workqueue(graid_dev->workqueue);
    destroy_workqueue(graid_dev->workqueue);
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