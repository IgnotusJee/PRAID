#ifndef _LIB_PCIEV_H
#define _LIB_PCIEV_H

#define PCIEV_VERSION 0x0110
#define PCIEV_DEVICE_ID PCIEV_VERSION
#define PCIEV_VENDOR_ID 0x0c51
#define PCIEV_SUBSYSTEM_ID 0x370d
#define PCIEV_SUBSYSTEM_VENDOR_ID PCIEV_VENDOR_ID

/* pcie 设备的bar资源，保留了物理地址前 1MB 的空间 */
struct __packed pciev_bar {
    // read only config
    uint32_t dev_cnt;

    struct __packed {
        uint64_t offset, size;
        uint64_t sector_sta;
        volatile uint32_t io_num, io_done;
        // 未进行的 io 操作区间为 (io_done, io_num]
    } io_property;

};

#define PTR_BAR_TO_STRIPE_O(addr) ((uint8_t*)(addr))
#define PTR_BAR_TO_STRIPE_N(addr) ((uint8_t*)(addr) + STRIPE_SIZE)
#define PTR_BAR_TO_STRIPE_V(addr) ((uint8_t*)(addr) + STRIPE_SIZE * 2)

#define U64_DATA(ptr, offset) (*(uint64_t*)((uint8_t*)(ptr) + (offset)))

#endif /* _LIB_PCIEV_H */