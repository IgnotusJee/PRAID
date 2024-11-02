#include <linux/pci.h>
#include <linux/module.h>
#include <linux/bio.h>

#include "nraid.h"
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

static void do_verify_work(struct work_struct *work) {
    struct verify_work* work_data = container_of(work, struct verify_work, work);
    struct verify_work_param* param = &work_data->param;

    VP_DEBUG("size=%llu, offset=%llu, num_sector=%llu\n", param->size, param->offset, param->num_sector);

    if(down_interruptible(&param->dev->sem) < 0) {
        VP_ERROR("Semens wait interrupted.\n");
        return;
    }

    if(!copy_page_to_buffer(param->page_old, PTR_BAR_TO_CHUNK_O(param->dev->chunk_addr), param->offset, param->size) || !copy_page_to_buffer(param->page_new, PTR_BAR_TO_CHUNK_N(param->dev->chunk_addr), param->offset, param->size)) {
        up(&param->dev->sem);
    }

    param->dev->bar->io_property.offset = param->offset;
    param->dev->bar->io_property.size = param->size;
    param->dev->bar->io_property.sector_sta = param->num_sector;

    param->dev->bar->io_property.io_num ++;

    if(param->page_new) {
        __free_page(param->page_new);
    }
    if(param->page_old) {
        __free_page(param->page_old);
    }
}

static bool add_verify_task(struct page *page_new, struct page *page_old, sector_t num_sector, uint64_t offset, uint64_t size, struct nraid_dev *dev) {
    struct verify_work* work = kmalloc(sizeof(struct verify_work), GFP_KERNEL);

    if(!work) {
        VP_ERROR("Alloc work struct failed.\n");
        return false;
    }

    work->param.page_new = alloc_page(GFP_KERNEL);
    if(!work->param.page_new) {
        VP_ERROR("Alloc new page failed.\n");
        goto out_work;
    }

    work->param.page_old = alloc_page(GFP_KERNEL);
    if(!work->param.page_old) {
        VP_ERROR("Alloc old page failed.\n");
        goto out_page;
    }

    copy_page_to_page(work->param.page_new, page_new);
    copy_page_to_page(work->param.page_old, page_old);

    work->param.num_sector = num_sector;
    work->param.offset = offset;
    work->param.size = size;
    work->param.dev = dev;
    INIT_WORK(&work->work, do_verify_work);
    
    if (!queue_work(dev->workqueue, &work->work)) {
        VP_ERROR("Add work failed.\n");
        goto out_page;
    }

    return true;

out_page:
    if(work->param.page_new) {
        __free_page(work->param.page_new);
    }
    if(work->param.page_old) {
        __free_page(work->param.page_old);
    }
out_work:
    kfree(work);
out_err:
    return false;
}

static void pciev_read_bio_endio(struct bio* bio_old) {
    struct bio* bio_new = bio_old->bi_private;
    struct bio_vec bvec_old, bvec_new;
	struct bvec_iter iter_old, iter_new;
    sector_t pos_sector = bio_new->bi_iter.bi_sector;
    
    if(bio_old->bi_vcnt != bio_new->bi_vcnt) {
        VP_ERROR("BIO format dose not match.\n");
        return;
    }

    for(iter_old = bio_new->bi_iter, iter_new = bio_new->bi_iter;
	    iter_old.bi_size && iter_new.bi_size &&
	    ((bvec_old = bio_iter_iovec(bio_old, iter_old)), 1) &&
        ((bvec_new = bio_iter_iovec(bio_new, iter_new)), 1);
	    bio_advance_iter_single(bio_old, &iter_old, bvec_old.bv_len),
    bio_advance_iter_single(bio_new, &iter_new, bvec_new.bv_len)) {
        BUG_ON(bvec_old.bv_len != bvec_new.bv_len);
        BUG_ON(bvec_old.bv_offset != bvec_new.bv_offset);
        VP_DEBUG("iter\n");
        add_verify_task(bvec_new.bv_page, bvec_old.bv_page, pos_sector, bvec_new.bv_offset, bvec_new.bv_len, nraid_dev);
        pos_sector += (bvec_new.bv_len >> KERNEL_SECTOR_SHIFT);
    }

    bio_for_each_segment(bvec_old, bio_old, iter_old)
		__free_page(bvec_old.bv_page);
    bio_put(bio_old);

    VP_DEBUG("read bio done.\n");

    submit_bio(bio_new);
}

struct bio* pcievdrv_submit_verify(struct bio *bio, unsigned int devi, struct nraid_dev *dev) {
    struct bio_vec bvec;
	struct bvec_iter iter;
    bool ret;
    sector_t pos_sector = bio->bi_iter.bi_sector;
    struct bio* n_bio;
    struct page* page;

    n_bio = bio_alloc(GFP_KERNEL, bio->bi_vcnt);
    bio_set_dev(n_bio, bio->bi_bdev);
    n_bio->bi_iter.bi_sector = bio->bi_iter.bi_sector;

    VP_DEBUG("outter, devi=%u\n", devi);
    bio_for_each_segment(bvec, bio, iter) {
        BUG_ON(bvec.bv_len > PAGE_SIZE);
        
        VP_DEBUG("inner, devi=%u\n", devi);
        page = alloc_page(GFP_KERNEL);

        if(!page) {
            VP_ERROR("alloc page failed.\n");
            goto out_bio;
        }

        if(!bio_add_page(n_bio, page, bvec.bv_len, bvec.bv_offset)) {
            VP_ERROR("add page failed.\n");
            goto out_page;
        }
    }

    n_bio->bi_private = bio;
    n_bio->bi_end_io = pciev_read_bio_endio;

    bio_set_op_attrs(n_bio, REQ_OP_READ, 0);

    return n_bio;
out_page:
    if(page) {
        __free_page(page);
    }
out_bio:
    bio_put(bio);
out_err:
    return ERR_PTR(-EIO);
}

static irqreturn_t pcievdrv_interrupt(int irq, void *dev_id) {
    struct nraid_dev *nraid_dev = (struct nraid_dev *)dev_id;
    VP_DEBUG("");
    up(&nraid_dev->sem);

    return IRQ_HANDLED;
}

static int pcievdrv_probe(struct pci_dev *dev, const struct pci_device_id *id) {
    int ret = 0;
    resource_size_t chunk_sta;
    size_t chunk_range;

    VP_INFO("probing function\n");

    if(pci_enable_device(dev)) {
        VP_ERROR("cannot enable device.\n");
        ret = -EIO;
        goto out_final;
    }

    nraid_dev->irq = dev->irq;
    if(nraid_dev->irq < 0) {
        VP_ERROR("invalid irq %d\n", nraid_dev->irq);
        ret = -EINVAL;
        goto out_final;
    }

    nraid_dev->mem_sta = pci_resource_start(dev, 0);
    nraid_dev->range = pci_resource_end(dev, 0) - nraid_dev->mem_sta + 1;
    VP_INFO("start %llx %lx\n", nraid_dev->mem_sta, nraid_dev->range);

    ret = pci_request_regions(dev, PCIEVIRT_DRV_NAME);
    if(ret) {
        VP_ERROR("PCI request regions err\n");
        ret = -EINVAL;
        goto out_final;
    }

    /* 将mem资源映射到虚拟地址 */
    nraid_dev->bar = memremap(nraid_dev->mem_sta, PAGE_SIZE, MEMREMAP_WT);

    if(!nraid_dev->bar) {
        VP_ERROR("bar memremap err.\n");
        ret = -ENOMEM;
        goto out_regions;
    }

    VP_INFO("bar memremap in: 0x%p\n", nraid_dev->bar);

    chunk_sta = nraid_dev->mem_sta + BAR_CHUNK_OFFSET;
    chunk_range = 3 * CHUNK_SIZE;

    sema_init(&nraid_dev->sem, 1);
    nraid_dev->workqueue = create_workqueue("verfy_task_work_queue");

    // if(nraid_dev->range < chunk_range + BAR_CHUNK_OFFSET) {
    //     VP_ERROR("request size larger than provided.\n");
    //     ret = -ENOMEM;
    //     goto out_regions;
    // }

    nraid_dev->chunk_addr = memremap(chunk_sta, chunk_range, MEMREMAP_WB);

    if(!nraid_dev->chunk_addr) {
        VP_ERROR("storage memremap err.\n");
        ret = -ENOMEM;
        goto out_memunmap_bar;
    }

    /* 申请中断IRQ并设定中断服务子函数 */
    ret = request_irq(nraid_dev->irq, pcievdrv_interrupt, IRQF_SHARED, PCIEVIRT_DRV_NAME, nraid_dev);
    if(ret) {
        VP_ERROR(KERN_ERR "Can't get assigned IRQ %d.\n", nraid_dev->irq);
        goto out_memunmap_sto;
    }

    pci_set_drvdata(dev, nraid_dev);
    VP_INFO("Probe succeeds.PCIE memory addr start at %llX, mypci->bar is 0x%p,interrupt No. %d.\n", nraid_dev->mem_sta, nraid_dev->bar, nraid_dev->irq);
    pcievdrv_get_configs(dev);

    return 0;

out_memunmap_sto:
    memunmap(nraid_dev->chunk_addr);
out_memunmap_bar:
    memunmap(nraid_dev->bar);
    destroy_workqueue(nraid_dev->workqueue);
out_regions:
    pci_release_regions(dev);
out_final:
    return ret;
}

static void pcievdrv_remove(struct pci_dev *dev) {
    struct nraid_dev *nraid_dev = pci_get_drvdata(dev);
    free_irq(nraid_dev->irq, nraid_dev);
    memunmap(nraid_dev->chunk_addr);
    memunmap(nraid_dev->bar);
    flush_workqueue(nraid_dev->workqueue);
    destroy_workqueue(nraid_dev->workqueue);
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