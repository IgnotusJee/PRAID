#ifndef __PCIEVDRV_H__
#define __PCIEVDRV_H__

#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>

#define PCIEVIRT_DRV_NAME "GRAID_PCIEDRV"

#define VP_INFO(string, args...) printk(KERN_INFO "%s: " string, PCIEVIRT_DRV_NAME, ##args)
#define VP_DEBUG(string, args...) printk(KERN_DEBUG "%s %s: " string, PCIEVIRT_DRV_NAME, __func__, ##args)
#define VP_ERROR(string, args...) printk(KERN_ERR "%s: " string, PCIEVIRT_DRV_NAME, ##args)

struct verify_work_param {
    struct page *page_new, *page_old;
    sector_t num_sector;
    uint64_t offset;
    uint64_t size;
    struct graid_dev *dev;
};

// work_struct 和 workqueue 函数的参数
struct verify_work {
    struct work_struct work;
    struct verify_work_param param;
};

static inline bool copy_page_to_buffer(struct page *page, char* buffer, size_t offset, size_t size) {
    char *data;

    if(!(data = kmap(page))) {
        return false;
    }

    memcpy(buffer + offset, data + offset, size);

    kunmap(page);
    return true;
}

static inline void copy_page_to_page(struct page *to, struct page *from) {
    void *to_data, *from_data;

    to_data = kmap(to);
    from_data = kmap(from);

    copy_page(to_data, from_data);

    kunmap(from);
    kunmap(to);
}

int pcievdrv_init(void);
void pcievdrv_exit(void);

#endif