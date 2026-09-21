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
bool diskfs_flush(void)     { return blk_flush(); }

bool diskfs_format(void) {
    if (!blk_present()) return false;

    /* Only ever the whole of an unpartitioned disk. If there is a table on
       it then every sector belongs to something somebody else wrote. */
    if (parts_count() > 0) {
        kprintf("  fs      refusing to format: the disk is partitioned\n");
        bb_log("fs refused to format a partitioned disk");
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
