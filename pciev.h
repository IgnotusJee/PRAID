#ifndef _LIB_PCIEV_H
#define _LIB_PCIEV_H

#define PCIEV_VERSION 0x0110
#define PCIEV_DEVICE_ID PCIEV_VERSION
#define PCIEV_VENDOR_ID 0x0c51
#define PCIEV_SUBSYSTEM_ID 0x370d
#define PCIEV_SUBSYSTEM_VENDOR_ID PCIEV_VENDOR_ID

/* pcie 设备的bar资源 */
struct __packed pciev_bar {

    // read only config for host
    struct __packed {
        uint32_t nr_hot_chunk;
        uint32_t nr_cold_chunk;
    } io_info;

    // rw config
    struct __packed {
        uint8_t rs_flag; // run stop control
    } io_control;

    struct __packed {
        uint64_t offset; // offset from start of bar, read only for both
    } io_bitmap;
};

#define PCIEV_BITMAP_START(bar) (void*)((uint8_t*)(bar) + (bar)->io_bitmap.offset)

#define U64_DATA(ptr, offset) (*(uint64_t*)((uint8_t*)(ptr) + (offset)))
#define BIT_TEST(ptr, pos) (((*((uint8_t*)(ptr) + ((pos) >> 3))) >> ((pos) & 0x7)) & 1)
#define BIT_SET(ptr, pos) ((*((uint8_t*)(ptr) + ((pos) >> 3))) ^= (1 << ((pos) & 0x7)))

#endif /* _LIB_PCIEV_H */