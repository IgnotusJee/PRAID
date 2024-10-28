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

void pcievdrv_set_wf(size_t chunk_num, struct graid_dev *dev) {
    if(!BIT_TEST(PCIEV_BITMAP_START(dev->bar), chunk_num)) {
        BIT_SET(PCIEV_BITMAP_START(dev->bar), chunk_num);
    }
    VP_DEBUG("set write flag in chuck num: %lu\n", chunk_num);
}

static irqreturn_t pcievdrv_interrupt(int irq, void *dev_id) {
    struct graid_dev *graid_dev = (struct graid_dev *)dev_id;
    VP_DEBUG("interrupt triggered.\n");

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
    graid_dev->bar = memremap(graid_dev->mem_sta, BAR_SIZE, MEMREMAP_WT);

    if(!graid_dev->bar) {
        VP_ERROR("bar memremap err.\n");
        ret = -ENOMEM;
        goto out_regions;
    }

    VP_INFO("bar memremap in: 0x%p\n", graid_dev->bar);

    /* 申请中断IRQ并设定中断服务子函数 */
    ret = request_irq(graid_dev->irq, pcievdrv_interrupt, IRQF_SHARED, PCIEVIRT_DRV_NAME, graid_dev);
    if(ret) {
        VP_ERROR(KERN_ERR "Can't get assigned IRQ %d.\n", graid_dev->irq);
        goto out_memunmap_bar;
    }

    pci_set_drvdata(dev, graid_dev);
    VP_INFO("Probe succeeds.PCIE memory addr start at %llX, mypci->bar is 0x%p,interrupt No. %d.\n", graid_dev->mem_sta, graid_dev->bar, graid_dev->irq);
    pcievdrv_get_configs(dev);

    return 0;

out_memunmap_bar:
    memunmap(graid_dev->bar);
out_regions:
    pci_release_regions(dev);
out_final:
    return ret;
}

static void pcievdrv_remove(struct pci_dev *dev) {
    struct graid_dev *graid_dev = pci_get_drvdata(dev);
    free_irq(graid_dev->irq, graid_dev);
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