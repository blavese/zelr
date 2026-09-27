/* Bringing the disk up.
 *
 * Files used to be held in memory and mirrored to the disk on every change.
 * Now the VFS reads and writes FAT directly, so all that is left here is
 * deciding which volume to use, preparing one when there is not one, and
 * recovering whatever an unclean shutdown stranded.
 *
 * "Which volume" is the part that grew. A disk image has one filesystem
 * written across the whole of it and sector zero is its boot sector; a real
 * disk is partitioned and sector zero is a partition table. Both have to
 * work, and the second one belongs to somebody.
 *
 * So the rules here are deliberately timid:
 *
 *   - A partition this kernel formatted is used, read and write. It is ours.
 *   - Any other FAT partition that is not the EFI System Partition is used
 *     as well. Somebody made it and it holds a filesystem we can read.
 *   - The EFI System Partition is never touched. It is how the machine
 *     boots, it is FAT so it would otherwise qualify, and a kernel that
 *     writes to it for scratch space is a kernel that stops the laptop
 *     starting.
 *   - An existing partition is never formatted. Formatting stays what it
 *     always was: something that happens to an unpartitioned disk with no
 *     filesystem on it, which is to say a blank image.
 */
#include "diskfs.h"
#include "fat.h"
#include "parts.h"
#include "blackbox.h"
#include "blockdev.h"
#include "printf.h"
#include "string.h"

/* A removable disk, mounted at /usb.
 *
 * parts.c reads the table of the disk the machine booted from and holds the
 * answer, so it is no use here. A stick is one of two shapes anyway: a
 * partition table with a FAT volume in it, which is what anything formatted
 * by Windows looks like, or a filesystem written across the whole device,
 * which is what a floppy looked like and what plenty of sticks still are.
 * Both are tried, in that order, and fat_mount_on decides: a sector that is
 * not a boot sector fails the checks it already makes.
 */
static const u8 FAT_TYPES[] = { 0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E, 0xEF };

static bool type_is_fat(u8 t) {
    for (u32 i = 0; i < sizeof FAT_TYPES; i++)
        if (FAT_TYPES[i] == t) return true;
    return false;
}

bool diskfs_mount_removable(u32 dev) {
    if (!blk_device_present(dev)) return false;

    u8 sec[SECTOR_SIZE];
    if (blk_read_on(dev, 0, 1, sec) && sec[510] == 0x55 && sec[511] == 0xAA) {
        for (u32 i = 0; i < 4; i++) {
            const u8 *e = sec + 446 + i * 16;
            u8 type = e[4];
            u32 start = (u32)e[8] | ((u32)e[9] << 8)
                      | ((u32)e[10] << 16) | ((u32)e[11] << 24);
            if (!start || !type_is_fat(type)) continue;
            if (fat_mount_on(FAT_VOL_USB, dev, start)) {
                bb_log("usb volume mounted from partition %d at sector %d",
                       i + 1, start);
                return true;
            }
        }
    }

    if (fat_mount_on(FAT_VOL_USB, dev, 0)) {
        bb_log("usb volume mounted from the whole device");
        return true;
    }

    bb_log("usb disk has no filesystem this kernel reads");
    return false;
}

void diskfs_unmount_removable(void) {
    fat_forget_volume(FAT_VOL_USB);
}

bool diskfs_removable_mounted(void) { return fat_mounted_on(FAT_VOL_USB); }

bool diskfs_available(void) { return blk_present(); }
bool diskfs_mounted(void)   { return fat_mounted(); }
/* Every disk with something mounted on it, not only the one the machine
   booted from. `sync`, fsync and switching off all come through here, and a
   stick is the disk most likely to be pulled out straight afterwards. */
bool diskfs_flush(void) {
    bool ok = blk_flush();
    if (fat_mounted_on(FAT_VOL_USB)) ok = fat_flush_volume(FAT_VOL_USB) && ok;
    return ok;
}

/* Whether anything has ever been written at the front or the back of the disk.
 *
 * "No partitions" was taken to mean a blank disk, and it does not. It is also
 * what a disk looks like when its table was there and was refused: a GPT
 * whose checksum does not add up, a header that could not be read, a
 * protective MBR with nothing this kernel trusts behind it. On a laptop
 * started from a stick, the disk being asked about is the internal drive, so
 * the first boot formatted somebody's drive because this kernel could not
 * read its table.
 *
 * So a disk is blank only when it looks blank. The first 128 KiB holds every
 * mark anything puts at the front -- an MBR or a boot sector, a GPT header
 * and its entries, the superblocks of the usual Linux filesystems, a volume
 * descriptor on a disc image -- and the last sector is where a GPT keeps its
 * spare header. All of it zero is a disk nobody has used. Anything else is
 * somebody's, whether or not this kernel can say whose. */
#define BLANK_SCAN_SECTORS 256

static bool disk_is_blank(void) {
    u8 sec[SECTOR_SIZE];
    u32 total = blk_sectors();
    u32 scan = total < BLANK_SCAN_SECTORS ? total : BLANK_SCAN_SECTORS;
    for (u32 s = 0; s < scan; s++) {
        if (!blk_read(s, 1, sec)) return false;
        for (u32 i = 0; i < SECTOR_SIZE; i++) if (sec[i]) return false;
    }
    if (total > scan) {
        if (!blk_read(total - 1, 1, sec)) return false;
        for (u32 i = 0; i < SECTOR_SIZE; i++) if (sec[i]) return false;
    }
    return true;
}

/* Formats the disk the machine booted from, when that cannot cost anybody
 * anything: it is blank, or what is on it is a volume this kernel has
 * mounted across the whole disk and so can already read -- somebody asking
 * for their zelr disk to be emptied. A partitioned disk, a table that was
 * refused, and a disk holding something unreadable are all left alone, and
 * the reason is said and logged. */
bool diskfs_format(void) {
    if (!blk_present()) return false;

    /* Only ever the whole of an unpartitioned disk. If there is a table on
       it then every sector belongs to something somebody else wrote. */
    if (parts_count() > 0) {
        kprintf("  fs      refusing to format: the disk is partitioned\n");
        bb_log("fs refused to format a partitioned disk");
        return false;
    }

    /* With no partitions, a mounted volume can only be one across the whole
       disk (diskfs_mount mounts nothing else then). */
    bool ours = fat_mounted_on(FAT_VOL_DISK);
    if (!ours && !disk_is_blank()) {
        const char *why = parts_gpt_error()[0]
            ? "it has a partition table this kernel could not trust"
            : "something is written on it that this kernel cannot read";
        kprintf("  fs      refusing to format: the disk is not blank, %s\n", why);
        bb_log("fs refused to format a disk that is not blank: %s", why);
        return false;
    }
    return fat_format("ZELR");
}

static int finish_mount(void) {
    /* Before anything else, because it is about whether this machine will
       start at all rather than about the files. A disk formatted by an
       earlier build carries the mark a BIOS acts on and nothing behind it,
       and nothing would ever format it again: it mounts, so it is kept.
       Saying so matters as much as doing it -- somebody whose machine died
       every other boot gets to see the line that explains the last week. */
    if (fat_boot_repair()) {
        kprintf("  fs      boot sector repaired, this disk was made without one\n");
        bb_log("fs boot sector repaired: the mark was there and no program behind it");
    }

    u32 stranded = fat_reclaim();
    if (stranded)
        kprintf("  fs      reclaimed %d cluster(s) from an unclean shutdown\n", stranded);
    return (int)fat_count("/");
}

int diskfs_mount(void) {
    if (!blk_present()) return -1;

    parts_scan();

    /* Why a table was refused is logged before anything is decided on the
       basis of there being no table. A rejected GPT and a disk that never
       had one both end up with no partitions, and those are very different
       situations: the second is a blank image, the first is a real disk
       whose table this kernel could not trust. Saying so is the difference
       between a puzzling boot and an obvious one. */
    const char *why = parts_gpt_error();
    if (why[0]) bb_log("parts gpt rejected: %s", why);

    if (parts_count() == 0) {
        /* No table, so the filesystem is the disk. This is the image case
           and the one every test runs. */
        bb_log("parts none, treating the disk as one volume");
        if (!fat_mount_at(0)) return -2;
        return finish_mount();
    }

    bb_log("parts %s, %d partition(s)", parts_scheme_name(), parts_count());

    /* Two passes, so a volume this kernel made wins over one that merely
       happens to be FAT. On a machine with both, ours is the one meant for
       us and the other is somebody's data. */
    for (int pass = 0; pass < 2; pass++) {
        for (u32 i = 0; i < parts_count(); i++) {
            const part_t *p = parts_get(i);
            if (!p->looks_like_fat) continue;
            if (p->efi_system) {
                if (pass == 0)
                    bb_log("parts skipping partition %d (%s): efi system partition",
                           i + 1, p->name);
                continue;
            }

            if (!fat_mount_at(p->start)) continue;

            bool ours = fat_is_zelr_volume();
            if (pass == 0 && !ours) continue;      /* keep looking for one of ours */

            kprintf("  fs      fat16 on partition %d (%s)%s\n",
                    i + 1, p->name, ours ? ", made by zelr" : "");
            bb_log("fs mounted partition %d at lba %d, %s",
                   i + 1, p->start, ours ? "ours" : "not ours");
            return finish_mount();
        }
    }

    bb_log("fs no usable fat partition among %d", parts_count());
    return -2;
}
