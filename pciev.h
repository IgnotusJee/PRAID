#ifndef _LIB_PCIEV_H
#define _LIB_PCIEV_H

#define PCIEV_VERSION 0x0110
#define PCIEV_DEVICE_ID PCIEV_VERSION
#define PCIEV_VENDOR_ID 0x0c51
#define PCIEV_SUBSYSTEM_ID 0x370d
#define PCIEV_SUBSYSTEM_VENDOR_ID PCIEV_VENDOR_ID

enum {
    DB_FREE = 0,    // device is free and waiting to receive task
    DB_WRITE = 1,   // device is receving task
    DB_BUSY = 2,    // device is doing task
    DB_DONE = 3     // device has task done and wait for writeback
};

/* pcie 设备的bar资源，保留了物理地址前 1MB 的空间 */
struct __packed pciev_bar {
    // read only config
    uint32_t dev_cnt;

    struct __packed {
        uint64_t offset, size;
        uint64_t sector_sta;
        uint8_t db;
    } io_property;

};

#define PTR_BAR_TO_STRIPE_O(addr) ((char*)(addr))
#define PTR_BAR_TO_STRIPE_N(addr) ((char*)(addr) + STRIPE_SIZE)
#define PTR_BAR_TO_STRIPE_V(addr) ((char*)(addr) + STRIPE_SIZE * 2)

#endif /* _LIB_PCIEV_H */