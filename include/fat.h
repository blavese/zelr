#pragma once
#include "types.h"

#define FAT_PATH_MAX 128

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
