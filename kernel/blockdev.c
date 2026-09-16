/* Which disk, and then which sectors of it.
 *
 * AHCI is tried first because that is what modern machines and modern virtual
 * machines present; NVMe next, which is what a laptop bought in the last
 * several years has instead of either of the others; the ATA PIO driver stays
 * as the fallback for QEMU's legacy default and for genuinely old hardware.
 * Whichever answers becomes disk zero, and everything written before there
 * was more than one disk still means that one.
 *
 * A stick plugged in later registers itself and gets a number of its own.
 */
#include "blockdev.h"
#include "ahci.h"
#include "ata.h"
#include "nvme.h"
#include "string.h"

static const blkdev_t *devices[BLK_MAX];

u32 blk_register(const blkdev_t *dev) {
    if (!dev) return BLK_NONE;
    for (u32 i = 0; i < BLK_MAX; i++) {
        if (!devices[i]) {
            devices[i] = dev;
            return i;
        }
    }
    return BLK_NONE;
}

void blk_unregister(u32 id) {
    if (id < BLK_MAX) devices[id] = 0;
}

u32 blk_count(void) {
    u32 n = 0;
    for (u32 i = 0; i < BLK_MAX; i++)
        if (devices[i]) n = i + 1;
    return n;
}

static const blkdev_t *dev_at(u32 id) {
    return id < BLK_MAX ? devices[id] : 0;
}

bool blk_device_present(u32 id)  { return dev_at(id) != 0; }

u32 blk_device_sectors(u32 id) {
    const blkdev_t *d = dev_at(id);
    return d && d->sectors ? d->sectors() : 0;
}

const char *blk_device_model(u32 id) {
    const blkdev_t *d = dev_at(id);
    return d && d->model ? d->model() : "";
}

const char *blk_device_driver(u32 id) {
    const blkdev_t *d = dev_at(id);
    return d ? d->driver : "none";
}

bool blk_device_removable(u32 id) {
    const blkdev_t *d = dev_at(id);
    return d ? d->removable : false;
}

/* How many sectors the driver underneath will take at once.
 *
 * They disagree, and by a lot: the AHCI driver stages through a single four
 * kilobyte buffer and so stops at eight, while the ATA one will take 255. A
 * caller asking for more than the driver allows is told false, which reads
 * as a disk error rather than as a request that wanted splitting, and it is
 * invisible on whichever of them happens to be in use on the machine being
 * tested. (Measured: the boot log asked for 32 sectors, worked on every ATA
 * machine, and silently never wrote a byte on any AHCI one, which is to say
 * on every machine made this century.) */
bool blk_read_on(u32 id, u32 lba, u32 count, void *buf) {
    const blkdev_t *d = dev_at(id);
    if (!d || !d->read || !count) return false;

    u32 run = d->max_run ? d->max_run() : count;
    if (!run) return false;

    u8 *p = (u8 *)buf;
    while (count) {
        u32 n = count < run ? count : run;
        if (!d->read(lba, n, p)) return false;
        lba += n;
        p += (u64)n * SECTOR_SIZE;
        count -= n;
    }
    return true;
}

bool blk_write_on(u32 id, u32 lba, u32 count, const void *buf) {
    const blkdev_t *d = dev_at(id);
    if (!d || !d->write || !count) return false;

    u32 run = d->max_run ? d->max_run() : count;
    if (!run) return false;

    const u8 *p = (const u8 *)buf;
    while (count) {
        u32 n = count < run ? count : run;
        if (!d->write(lba, n, p)) return false;
        lba += n;
        p += (u64)n * SECTOR_SIZE;
        count -= n;
    }
    return true;
}

bool blk_flush_on(u32 id) {
    const blkdev_t *d = dev_at(id);
    return d && d->flush ? d->flush() : false;
}

/* --- the disk the machine booted from ------------------------------------ */

static const blkdev_t AHCI_DEV = {
    "ahci", ahci_read, ahci_write, ahci_flush,
    ahci_sectors, ahci_max_run, ahci_model, false
};
static const blkdev_t NVME_DEV = {
    "nvme", nvme_read, nvme_write, nvme_flush,
    nvme_sectors, nvme_max_run, nvme_model, false
};
static const blkdev_t ATA_DEV = {
    "ata", ata_read, ata_write, ata_flush,
    ata_sectors, ata_max_run, ata_model, false
};

bool blk_init(void) {
    /* Tried in the order that changes least, so every machine that worked
       before this picks the same controller it always did. */
    const blkdev_t *found = 0;
    if (ahci_init())      found = &AHCI_DEV;
    else if (nvme_init()) found = &NVME_DEV;
    else if (ata_init())  found = &ATA_DEV;

    devices[BLK_BOOT] = found;
    return found != 0;
}

bool blk_present(void)        { return blk_device_present(BLK_BOOT); }
u32  blk_sectors(void)        { return blk_device_sectors(BLK_BOOT); }
const char *blk_model(void)   { return blk_device_model(BLK_BOOT); }
const char *blk_driver(void)  { return blk_device_driver(BLK_BOOT); }

bool blk_read(u32 lba, u32 count, void *buf) {
    return blk_read_on(BLK_BOOT, lba, count, buf);
}
bool blk_write(u32 lba, u32 count, const void *buf) {
    return blk_write_on(BLK_BOOT, lba, count, buf);
}
bool blk_flush(void) { return blk_flush_on(BLK_BOOT); }
