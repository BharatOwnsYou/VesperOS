#include "blockdev.h"
#include "string.h"

static blkdev_t devs[BLK_MAX_DEVS];
static int ndevs = 0;
static int writes_on = 0;

int blk_register(const blkdev_t *d) {
    if (ndevs >= BLK_MAX_DEVS) return -1;
    devs[ndevs] = *d;
    return ndevs++;
}

int blk_count(void) { return ndevs; }

blkdev_t *blk_get(int i) {
    if (i < 0 || i >= ndevs) return NULL;
    return &devs[i];
}

blkdev_t *blk_find(const char *name) {
    for (int i = 0; i < ndevs; i++)
        if (k_streq(devs[i].name, name)) return &devs[i];
    return NULL;
}

/* count > sectors first so the second compare can't overflow */
static int range_ok(const blkdev_t *d, u64 lba, u32 count) {
    if (count == 0) return 0;
    if ((u64)count > d->sectors) return 0;
    return lba <= d->sectors - count;
}

int blk_read(blkdev_t *d, u64 lba, u32 count, void *buf) {
    if (!d || !d->read || !buf || !range_ok(d, lba, count)) return BLK_ERANGE;
    return d->read(d, lba, count, buf);
}

int blk_write(blkdev_t *d, u64 lba, u32 count, const void *buf) {
    if (!writes_on) return BLK_EPERM;
    if (!d || !d->write || !buf || !range_ok(d, lba, count)) return BLK_ERANGE;
    return d->write(d, lba, count, buf);
}

void blk_set_writes_enabled(int on) { writes_on = on ? 1 : 0; }
int  blk_writes_enabled(void) { return writes_on; }
