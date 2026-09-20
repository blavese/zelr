#pragma once
#include "types.h"

#define FAT_PATH_MAX 128

/* The longest name this reads or writes. Sixty three characters covers
   anything anybody types; the limit exists because a directory entry
   carries thirteen at a time and something has to bound the loop. */
#define FAT_NAME_MAX 64

/* Two volumes: the disk the machine booted from, and something removable.
   Enough to copy a file from one to the other, which is the point. */
#define FAT_VOLUMES   2
#define FAT_VOL_DISK  0
#define FAT_VOL_USB   1

/* Which volume the calls below are about. Selected at the edge, in
   kernel/vfs.c, from the path. */
void fat_select(u32 vol);
u32  fat_selected(void);
bool fat_mounted_on(u32 vol);
void fat_forget_volume(u32 vol);

/* Mounts a volume of a given disk at a given sector. */
bool fat_mount_on(u32 vol, u32 dev, u32 base_lba);

/* Mounts the volume starting at this sector of the disk. Everything inside
   is relative to it, so a filesystem in a partition and one written across
   a whole disk are the same code with a different base. fat_mount is the
   unpartitioned case and is what a disk image needs. */
bool fat_mount_at(u32 base_lba);
bool fat_mount(void);
bool fat_mounted(void);

/* Whether the mounted volume is one this kernel formatted, which is what
   the OEM name and volume serial in the boot sector say. Used to prefer
   our own volume over one that is merely FAT. */
bool fat_is_zelr_volume(void);

/* The same question, and where the log lives, asked of a boot sector that
   has been read rather than of the volume that is mounted. The black box
   needs both before anything is mounted and from the fault path, and the
   answers differ between the two widths. */
bool fat_boot_is_ours(const u8 *boot);
u32  fat_boot_log_lba(const u8 *boot);
u32  fat_boot_reserved(const u8 *boot);

/* 16 or 32, decided from the cluster count rather than the label. */
u32  fat_type(void);
u32  fat_base(void);                    /* where the mounted volume starts */

/* Formats at a base, which for anything but a raw image is the start of a
   partition. Refuses to write outside `sectors` from there. */
bool fat_format_at(u32 base_lba, u32 sectors, const char *label);
bool fat_format(const char *label);

/* Listing a directory. `path` is absolute; "/" is the root. Returns 1 when
   an entry was produced, 0 past the end, -1 if the path is not a directory. */
int  fat_list(const char *path, u32 index, char *name_out, u32 *size_out, bool *dir_out);
u32  fat_count(const char *path);

int  fat_read_file(const char *path, u8 *buf, u32 cap);   /* bytes, or -1 */
bool fat_write_file(const char *path, const u8 *buf, u32 size);
bool fat_delete_file(const char *path);

/* True if the path exists. Fills in what it is, if asked. */
bool fat_stat(const char *path, u32 *size_out, bool *dir_out);

bool fat_mkdir(const char *path);
bool fat_rmdir(const char *path);          /* only when empty */

u32  fat_total_clusters(void);
u32  fat_cluster_bytes(void);
u32  fat_free_bytes(void);

/* Releases clusters no directory entry points at, which is what a crash
   between writing a file and committing it leaves behind. Returns the
   number recovered. */
u32  fat_reclaim(void);
