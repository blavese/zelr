/* Picks a disk controller and hides which one it was from the filesystem.
 *
 * AHCI is tried first because that is what modern machines and modern virtual
 * machines present; the ATA PIO driver stays as the fallback for QEMU's
 * legacy default and for genuinely old hardware. */
#include "blockdev.h"
#include "ahci.h"
#include "ata.h"
#include "nvme.h"

typedef enum { DISK_NONE, DISK_AHCI, DISK_NVME, DISK_ATA } disk_t;
static disk_t disk = DISK_NONE;

/* Tried in the order that changes least.
 *
 * AHCI stays first, so every machine that worked before this still picks the
 * same controller it always did. NVMe comes next, which is what a laptop
 * made in the last several years has instead of either of the other two, and
 * on such a machine the first two probes simply find nothing. ATA remains
 * last: it is the fallback for genuinely old hardware and for QEMU's legacy
 * default, and it is the only one of the three that needs no PCI at all. */
bool blk_init(void) {
    if (ahci_init()) { disk = DISK_AHCI; return true; }
    if (nvme_init()) { disk = DISK_NVME; return true; }
    if (ata_init())  { disk = DISK_ATA;  return true; }
    disk = DISK_NONE;
    return false;
}

bool blk_present(void) {
    switch (disk) {
        case DISK_AHCI: return ahci_present();
        case DISK_NVME: return nvme_present();
        case DISK_ATA:  return ata_present();
        default:        return false;
    }
}

u32 blk_sectors(void) {
    switch (disk) {
        case DISK_AHCI: return ahci_sectors();
        case DISK_NVME: return nvme_sectors();
        case DISK_ATA:  return ata_sectors();
        default:        return 0;
    }
}

const char *blk_model(void) {
    switch (disk) {
        case DISK_AHCI: return ahci_model();
        case DISK_NVME: return nvme_model();
        case DISK_ATA:  return ata_model();
        default:        return "";
    }
}

const char *blk_driver(void) {
    switch (disk) {
        case DISK_AHCI: return "ahci";
        case DISK_NVME: return "nvme";
        case DISK_ATA:  return "ata";
        default:        return "none";
    }
}

/* How many sectors the driver underneath will take at once.
 *
 * They disagree, and by a lot: the AHCI driver stages through a single four
 * kilobyte buffer and so stops at eight, while the ATA one will take 255. A
 * caller asking for more than the driver allows is told false, which reads
 * as a disk error rather than as a request that wanted splitting, and it is
 * invisible on whichever of the two happens to be in use on the machine
 * being tested. (Measured: the boot log asks for 32 sectors, worked on every
 * ATA machine, and silently never wrote a byte on any AHCI one, which is to
 * say on every machine made this century.) Splitting belongs here because
 * here is the only place it can be done once. */
static u32 max_run(void) {
    switch (disk) {
        case DISK_AHCI: return ahci_max_run();
        case DISK_NVME: return nvme_max_run();
        case DISK_ATA:  return ata_max_run();
        default:        return 0;
    }
}

static bool one_read(u32 lba, u32 n, void *buf) {
    switch (disk) {
        case DISK_AHCI: return ahci_read(lba, n, buf);
        case DISK_NVME: return nvme_read(lba, n, buf);
        case DISK_ATA:  return ata_read(lba, n, buf);
        default:        return false;
    }
}

static bool one_write(u32 lba, u32 n, const void *buf) {
    switch (disk) {
        case DISK_AHCI: return ahci_write(lba, n, buf);
        case DISK_NVME: return nvme_write(lba, n, buf);
        case DISK_ATA:  return ata_write(lba, n, buf);
        default:        return false;
    }
}

bool blk_read(u32 lba, u32 count, void *buf) {
    u32 run = max_run();
    if (!run || count == 0) return false;

    u8 *p = (u8 *)buf;
    while (count) {
        u32 n = count < run ? count : run;
        if (!one_read(lba, n, p)) return false;
        lba += n;
        p += (u64)n * SECTOR_SIZE;
        count -= n;
    }
    return true;
}

bool blk_write(u32 lba, u32 count, const void *buf) {
    u32 run = max_run();
    if (!run || count == 0) return false;

    const u8 *p = (const u8 *)buf;
    while (count) {
        u32 n = count < run ? count : run;
        if (!one_write(lba, n, p)) return false;
        lba += n;
        p += (u64)n * SECTOR_SIZE;
        count -= n;
    }
    return true;
}

bool blk_flush(void) {
    switch (disk) {
        case DISK_AHCI: return ahci_flush();
        case DISK_NVME: return nvme_flush();
        case DISK_ATA:  return ata_flush();
        default:        return false;
    }
}
