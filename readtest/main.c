/*
    transfer and prind head data for a block device
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/gfp.h>
#include <linux/uaccess.h>

static struct block_device *bdev;
static struct page *page;
static sector_t start_sector = 0;

void submit_my_bio(int id, struct block_device* bdev) {
    struct bio *bio;
    int ret;
    uint64_t i;
    uint8_t *data;

    bio = bio_alloc(GFP_KERNEL, 1);
    if (!bio) {
        pr_alert("Failed to allocate bio\n");
        return;
    }

    bio_set_dev(bio, bdev);
    bio->bi_iter.bi_sector = start_sector;

    data = kmap(page);
    if(!data) {
        pr_alert("Failed to map page\n");
        return;
    }
    kunmap(page);
    
    ret = bio_add_page(bio, page, PAGE_SIZE, 0);
    if(ret != PAGE_SIZE) {
        pr_alert("Failed to add page to bio\n");
        bio_put(bio);
        return;
    }

    bio_set_op_attrs(bio, REQ_OP_READ, 0);
    submit_bio_wait(bio);

    printk(KERN_DEBUG "in bdev %d: %p\n", id, bdev);
    data = kmap(page);
    for(i = 0; i < PAGE_SIZE; i += 16) {
        printk(KERN_DEBUG "%08llx | %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx |\n",
            i,
            *(uint8_t*)(data + i),
            *(uint8_t*)(data + i + 1),
            *(uint8_t*)(data + i + 2),
            *(uint8_t*)(data + i + 3),
            *(uint8_t*)(data + i + 4),
            *(uint8_t*)(data + i + 5),
            *(uint8_t*)(data + i + 6),
            *(uint8_t*)(data + i + 7),
            *(uint8_t*)(data + i + 8),
            *(uint8_t*)(data + i + 9),
            *(uint8_t*)(data + i + 10),
            *(uint8_t*)(data + i + 11),
            *(uint8_t*)(data + i + 12),
            *(uint8_t*)(data + i + 13),
            *(uint8_t*)(data + i + 14),
            *(uint8_t*)(data + i + 15));
    }
    kunmap(page);
}

static int __init my_init(void) {
    int i;

    for(i = 0; i < 4; i ++) {
        bdev = blkdev_get_by_dev(MKDEV(259, i), FMODE_READ | FMODE_WRITE, NULL);
        if(IS_ERR(bdev)) {
            pr_alert("Failed to open block device\n");
            return PTR_ERR(bdev);
        }

        page = alloc_page(GFP_KERNEL);
        if(!page) {
            pr_alert("Failed to allocate page\n");
            blkdev_put(bdev, FMODE_READ | FMODE_WRITE);
            return -ENOMEM;
        }

        submit_my_bio(i, bdev);
        blkdev_put(bdev, FMODE_READ | FMODE_WRITE);
    }

    return 0; // 成功加载模块
}

static void __exit my_exit(void) {
    if (page)
        __free_page(page);
    pr_alert("Module unloaded.\n");
}

module_init(my_init);
module_exit(my_exit);

MODULE_LICENSE("GPL v2");