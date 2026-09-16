#pragma once
#include "types.h"

#define SECTOR_SIZE 512

/* Disks.
 *
 * There used to be one. blk_read took a sector number and there was no way to
 * say which disk it was on, because there was only ever the one the machine
 * booted from, and everything above this was written that way. That is why a
 * USB stick could be read a sector at a time by a shell command and could not
 * be mounted: the filesystem had no way to ask for it.
 *
 * So a disk is now a thing with an identity. The boot disk is number zero and
 * keeps the old calls, which mean what they always did. Anything that arrives
 * later, which so far means a USB stick, registers itself and gets a number of
 * its own. */

#define BLK_MAX     4
#define BLK_BOOT    0
#define BLK_NONE    0xFFFFFFFFu

/* What a driver has to provide. Each of these drivers is a single device, so
   none of them needs telling which one it is. */
typedef struct {
    const char *driver;
    bool (*read)(u32 lba, u32 count, void *buf);
    bool (*write)(u32 lba, u32 count, const void *buf);
    bool (*flush)(void);
    u32  (*sectors)(void);
    u32  (*max_run)(void);          /* sectors it will take in one request */
    const char *(*model)(void);
    bool removable;
} blkdev_t;

/* Takes a number, or BLK_NONE when there is no room. The structure is kept,
   not copied, so it has to outlive the registration. */
u32  blk_register(const blkdev_t *dev);
void blk_unregister(u32 id);

u32  blk_count(void);                      /* the highest number in use, plus one */
bool blk_device_present(u32 id);
u32  blk_device_sectors(u32 id);
const char *blk_device_model(u32 id);
const char *blk_device_driver(u32 id);
bool blk_device_removable(u32 id);

/* Reading and writing a particular one. Requests longer than the driver will
   take are split here, because here is the only place it can be done once. */
bool blk_read_on(u32 id, u32 lba, u32 count, void *buf);
bool blk_write_on(u32 id, u32 lba, u32 count, const void *buf);
bool blk_flush_on(u32 id);

/* The disk the machine booted from, which is what everything above this
   meant before there was more than one. */
bool blk_init(void);
bool blk_present(void);
u32  blk_sectors(void);
const char *blk_model(void);
const char *blk_driver(void);
bool blk_read(u32 lba, u32 count, void *buf);
bool blk_write(u32 lba, u32 count, const void *buf);
bool blk_flush(void);
