#ifndef VESPEROS_MFS_H
#define VESPEROS_MFS_H
#include "../include/types.h"

/* MFS (Mystical File System): serialization format for the VFS tree. fixed header, flat record table
 * (name, type, parent index, offset+length into the blob), then the data blob, all in one contiguous image.
 *
 * limitation: no disk/block driver yet (no AHCI, no NVMe) so mfs_save() only writes into a static RAM buffer.
 * it doesn't touch your nvme and the image doesn't survive a reboot. a block driver would reuse this:
 * mfs_save() then a raw write, and a raw read then a loader (not written yet) at boot.
 * VFS_PROGRAM nodes aren't saved, they're code in the kernel image re-registered via vpkg at boot */

#define MFS_MAGIC0 'M'
#define MFS_MAGIC1 'F'
#define MFS_MAGIC2 'S'
#define MFS_MAGIC3 '1'

typedef struct __attribute__((packed)) {
    char name[32];
    u8   type;        /* 0 = directory, 1 = file */
    s32  parent_idx;  /* -1 for the root record */
    u32  data_offset; /* offset into the data blob, valid for type==1 */
    u32  data_len;
} mfs_record_t;

typedef struct __attribute__((packed)) {
    char magic[4];    /* "MFS1" */
    u32  version;
    u32  record_count;
    u32  data_size;
} mfs_header_t;

/* serializes the live VFS tree into the static image buffer. returns the image size in bytes */
u32 mfs_save(void);

u32 mfs_image_size(void);
u32 mfs_record_count(void);

#endif
