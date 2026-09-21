#pragma once
#include "types.h"
#include "fat.h"

/* The filesystem everything else talks to.
 *
 * Underneath there are three sources, checked in this order:
 *
 *   1. the live tree, which is /sys and /bin: files that are generated when
 *      they are read, and the programs built into the kernel image
 *   2. the FAT16 volume, when a disk is present
 *   3. an in-memory filesystem, when there is no disk at all
 *
 * Callers see one namespace with directories and absolute paths, and never
 * find out which of the three answered. */

#define VFS_PATH_MAX FAT_PATH_MAX
#define VFS_NAME_MAX FAT_NAME_MAX
#define VFS_MAX_OPEN 16


/* Flags for vfs_open. */
#define O_READ   0x01
#define O_WRITE  0x02
#define O_CREATE 0x04
#define O_TRUNC  0x08
#define O_APPEND 0x10

void vfs_init(void);

/* Turns whatever the caller typed into a clean absolute path, resolving "."
   and ".." and applying the current task's working directory. */
bool vfs_resolve(const char *in, char *out, u32 cap);

/* Path operations. Every path may be relative; each resolves it first. */
int  vfs_list(const char *path, u32 index, char *name_out, u32 *size_out, bool *dir_out);
u32  vfs_count(const char *path);
bool vfs_stat(const char *path, u32 *size_out, bool *dir_out);
int  vfs_read(const char *path, void *buf, u32 cap);       /* bytes, or -1 */
bool vfs_write(const char *path, const void *buf, u32 len);
bool vfs_append(const char *path, const void *buf, u32 len);
bool vfs_delete(const char *path);

/* A different name for a file, within one directory on the disk. False
   for anything else, including across directories: see kernel/fat.c for
   why that one cannot be done safely here. */
bool vfs_rename(const char *from, const char *to);
bool vfs_mkdir(const char *path);
bool vfs_rmdir(const char *path);

/* The working directory of the running task. */
const char *vfs_cwd(void);
bool vfs_chdir(const char *path);

/* True for a path the filesystem makes up as it goes, or holds its own copy
   of: /sys, and /bin. Nothing may be written to one. */
bool vfs_generated(const char *abs);

/* Open files are in include/fd.h. They are a process's business rather than
   the filesystem's, which is why they left this header. */

/* Reading a whole file into a buffer the caller must free. */
u8  *vfs_slurp(const char *path, u32 *size_out);

/* Where the built-in programs are registered from. They appear in /bin. */
void vfs_add_builtin(const char *name, const u8 *data, u32 size);
u32  vfs_builtin_count(void);

bool vfs_disk_backed(void);
