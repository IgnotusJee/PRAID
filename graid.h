#ifndef __GRAID_H__
#define __GRAID_H__

#include <linux/blkdev.h>
#include <linux/semaphore.h>

#define GRAID_NAME "graid"
#define GRAID_ERROR(string, args...) printk(KERN_ERR "%s: " string, GRAID_NAME, ##args)
#define GRAID_INFO(string, args...) printk(KERN_INFO "%s: " string, GRAID_NAME, ##args)

#define KERNEL_SECTOR_SIZE 512
#define HARDSECT_SIZE 512
#define KERNEL_SECTOR_SHIFT 9

#define CHUNK_SHIFT 12
#define CHUNK_SIZE (1 << CHUNK_SHIFT)

#define SECTORS_IN_CHUNK 8
#define SECTORS_IN_CHUNK_SHIFT 3

#define KB(k) ((k) << 10)
#define MB(m) ((m) << 20)
#define GB(g) ((g) << 30)

#define BYTE_TO_KB(b) ((b) >> 10)
#define BYTE_TO_MB(b) ((b) >> 20)
#define BYTE_TO_GB(b) ((b) >> 30)

#define BAR_CHUNK_OFFSET MB(1)

struct graid_config {
    unsigned int nr_nvme_disks;
    uint64_t size_nvme_disk;
    unsigned int nvme_major;
    unsigned int nvme_minor_verify;
    unsigned int nvme_minor[32];
};

struct graid_dev {
    struct graid_config config;

    // disk property
    uint64_t size;
    unsigned int disk_cnt;
    struct block_device *bdev[32]; // 下级的数据盘
    struct block_device *bdev_verify; // 校验盘

    // block device
    // spinlock_t blk_lock; // unused
    struct request_queue *queue;
    struct gendisk *gd;
    struct semaphore sem;
    struct workqueue_struct *workqueue;
    // wait_queue_head_t verify_wait_queue; // 用于等待上一个校验任务结束的等待队列

    // pcie device
    resource_size_t mem_sta;
    size_t range;
    struct pciev_bar __iomem *bar; // struct pciev_bar 存放的地址
    void __iomem *chunk_addr; // 三个chunk计算区域的的起始地址
    int irq;
};

enum {
    STATUS_FREE = 0,    // device is free and waiting to receive task
    STATUS_WRITE = 1,   // device is receving task
};

extern struct graid_dev* graid_dev;

#endif