#ifndef VESPEROS_BLOCKDEV_H
#define VESPEROS_BLOCKDEV_H
#include "../include/types.h"

/* generic block devices. storage drivers (ahci, nvme) register one blkdev_t per drive, everything
 * above (shell, installer) only talks to this layer. polled, synchronous, one request at a time */

#define BLK_MAX_DEVS 8

#define BLK_OK      0
#define BLK_EIO    -1   /* device error or timeout */
#define BLK_ERANGE -2   /* bad lba/count/buffer */
#define BLK_EPERM  -3   /* writes are locked, see blk_set_writes_enabled() */

typedef struct blkdev {
    char name[8];                 /* "sata0", "nvme0" */
    char model[41];
    u64  sectors;                 /* total logical sectors */
    u32  sector_size;             /* bytes per logical sector (512 or 4096) */
    int  (*read)(struct blkdev *d, u64 lba, u32 count, void *buf);
    int  (*write)(struct blkdev *d, u64 lba, u32 count, const void *buf);
    void *priv;                   /* driver state */
} blkdev_t;

int       blk_register(const blkdev_t *d);   /* copies d, returns its index or -1 if the table is full */
int       blk_count(void);
blkdev_t *blk_get(int i);
blkdev_t *blk_find(const char *name);

int blk_read(blkdev_t *d, u64 lba, u32 count, void *buf);
int blk_write(blkdev_t *d, u64 lba, u32 count, const void *buf);

/* writes are OFF at boot and every blk_write() fails with BLK_EPERM until something turns them on.
 * only code that has already asked the user for an explicit confirmation should do that */
void blk_set_writes_enabled(int on);
int  blk_writes_enabled(void);

#endif
