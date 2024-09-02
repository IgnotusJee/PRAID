#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/gfp.h>
#include <linux/uaccess.h>

static struct block_device *bdev;
static struct page *page, *page1;
static sector_t start_sector = 0;

#define READ_SIZE 3072

static void my_bio_end_io(struct bio *bio)
{
    int err = bio->bi_status; // 获取错误状态

    if (err)
        pr_alert("Bio completed with error %d\n", err);
    else
        pr_alert("Bio completed successfully\n");

    /* 释放bio */
    bio_put(bio);
}

void submit_my_bio(void)
{
    struct bio_vec bvec;
	struct bvec_iter iter;
    struct bio *bio;
    int ret;
    char *data, *data1;

    /* 分配一个bio，不分段 */
    bio = bio_alloc(GFP_KERNEL, 1);
    if (!bio)
    {
        pr_alert("Failed to allocate bio\n");
        return;
    }

    /* 设置bio的参数 */
    bio_set_dev(bio, bdev);
    bio->bi_iter.bi_sector = start_sector; // 设置起始扇区
    bio->bi_end_io = my_bio_end_io;        // 设置完成回调

    /* 获取page的虚拟地址，并填充数据 */
    data = kmap(page);
    if (data)
    {
        strcpy(data, "Hello, block device!"); // 填充数据，确保不超过PAGE_SIZE
        kunmap(page);
    }
    else {
        printk(KERN_INFO "page null\n");
    }

    data1 = kmap(page1);
    if (data1)
    {
        strcpy(data1, "Hello, block device!"); // 填充数据，确保不超过PAGE_SIZE
        kunmap(page1);
    }
    else {
        printk(KERN_INFO "page null\n");
    }

    /* 向bio添加页 */
    ret = bio_add_page(bio, page, READ_SIZE, 0);
    if (ret != READ_SIZE)
    {
        pr_alert("Failed to add page to bio\n");
        bio_put(bio);
        return;
    }
    ret = bio_add_page(bio, page1, READ_SIZE, 0);
    if (ret != READ_SIZE)
    {
        pr_alert("Failed to add page to bio\n");
        bio_put(bio);
        return;
    }

    /* 设置操作类型和属性 */
    bio_set_op_attrs(bio, REQ_OP_WRITE, 0); // 设置为读操作，如果是写操作应该使用REQ_OP_WRITE

    // bio_for_each_segment(bvec, bio, iter)  {
    //     printk(KERN_INFO "%llu, %u, %u, %u\n", bio->bi_iter.bi_sector, bio->bi_iter.bi_size, bio->bi_iter.bi_idx, bio->bi_iter.bi_bvec_done);
    // }


    /* 提交bio */
    submit_bio(bio);
}

static int __init my_init(void)
{
    /* 获取块设备 */
    bdev = blkdev_get_by_dev(MKDEV(259, 0), FMODE_READ | FMODE_WRITE, NULL);
    if (IS_ERR(bdev))
    {
        pr_alert("Failed to open block device\n");
        return PTR_ERR(bdev);
    }

    /* 分配一个页 */
    page = alloc_page(GFP_KERNEL);
    page1 = alloc_page(GFP_KERNEL);
    if (!page)
    {
        pr_alert("Failed to allocate page\n");
        blkdev_put(bdev, FMODE_READ | FMODE_WRITE);
        return -ENOMEM;
    }

    /* 选择一个示例扇区 */
    start_sector = 2; // 示例扇区，需要根据实际情况调整

    /* 提交bio */
    submit_my_bio();

    return 0; // 成功加载模块
}

static void __exit my_exit(void)
{
    if (page)
        __free_page(page);
    if (page1)
        __free_page(page1);
    if (bdev)
        blkdev_put(bdev, FMODE_READ | FMODE_WRITE);
    pr_alert("Module unloaded.\n");
}

module_init(my_init);
module_exit(my_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple example of bio submission in a Linux kernel module with data filling, adapted for newer kernel APIs.");
