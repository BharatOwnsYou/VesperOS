#ifndef VESPEROS_DISKDETECT_H
#define VESPEROS_DISKDETECT_H
#include "../include/types.h"

typedef enum {
    DISK_CTRL_NVME,
    DISK_CTRL_AHCI,
    DISK_CTRL_IDE,
    DISK_CTRL_RAID,
    DISK_CTRL_OTHER,
} disk_ctrl_kind_t;

typedef struct {
    disk_ctrl_kind_t kind;
    u8 bus, slot, func;
    u16 vendor_id, device_id;
} disk_ctrl_t;

void disk_detect_scan(void);
int  disk_detect_count(void);
const disk_ctrl_t *disk_detect_at(int i);
const char *disk_ctrl_kind_name(disk_ctrl_kind_t k);

#endif
