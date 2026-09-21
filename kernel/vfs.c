/* The filesystem everything else talks to.
 *
 * There are three sources of files and one namespace over them. The live
 * tree comes first and is read-only; then the FAT volume if a disk is
 * present; then an in-memory filesystem if it is not, so that a machine with
 * no disk still works rather than half working.
 *
 * The path handling is the fiddly part and it is all here rather than spread
 * around: everything below resolves a caller's path exactly once, at the
 * front door, and works in absolute paths after that.
 */
#include "vfs.h"
#include "sysfs.h"
#include "fat.h"
#include "fs.h"
#include "blockdev.h"
#include "sched.h"
#include "heap.h"
#include "string.h"
#include "printf.h"

/* --- the live tree ------------------------------------------------------ */

/* Programs built into the kernel image are files in /bin, which the live
   tree owns; this is only the door they come in through. */
void vfs_add_builtin(const char *name, const u8 *data, u32 size) {
    sysfs_add_program(name, data, size);
}

u32 vfs_builtin_count(void) { return sysfs_program_count(); }

/* Paths the live tree answers for. Nothing written to the disk can shadow
   one, and nothing here can be written to. */
/* Paths the filesystem generates or the kernel carries its own copy of.
   Neither can be written to, and fd.c has to ask as well, which is why this
   is no longer private to this file. */
bool vfs_generated(const char *abs) {
    return sysfs_owns(abs) ||
           (strncmp(abs, "/bin", 4) == 0 && (abs[4] == 0 || abs[4] == '/'));
}

bool vfs_disk_backed(void) { return fat_mounted(); }

/* --- paths -------------------------------------------------------------- */

static char cwd_fallback[VFS_PATH_MAX] = "/";

const char *vfs_cwd(void) {
    task_t *t = task_current();
    if (t && t->cwd[0]) return t->cwd;
    return cwd_fallback;
}

static void set_cwd(const char *abs) {
    task_t *t = task_current();
    if (t) { strncpy(t->cwd, abs, VFS_PATH_MAX - 1); t->cwd[VFS_PATH_MAX - 1] = 0; }
    else   { strncpy(cwd_fallback, abs, VFS_PATH_MAX - 1); cwd_fallback[VFS_PATH_MAX - 1] = 0; }
}

/* Builds a clean absolute path: relative input is joined to the working
   directory, then "." is dropped and ".." pops a component. The result never
   contains a repeated or trailing slash, so two spellings of the same place
   compare equal. */
bool vfs_resolve(const char *in, char *out, u32 cap) {
    if (!in || cap < 2) return false;

    char work[VFS_PATH_MAX * 2];
    u32 n = 0;

    if (in[0] != '/') {
        const char *base = vfs_cwd();
        while (base[n] && n < sizeof(work) - 2) { work[n] = base[n]; n++; }
        if (n == 0 || work[n - 1] != '/') work[n++] = '/';
    }
    for (u32 i = 0; in[i] && n < sizeof(work) - 1; i++) work[n++] = in[i];
    work[n] = 0;

    /* Walk the components, keeping the offsets where each one starts so ".."
       can drop the previous one. */
    u32 starts[32];
    u32 depth = 0;
    u32 len = 1;
    out[0] = '/';

    const char *p = work;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;

        char part[VFS_NAME_MAX];
        u32 k = 0;
        while (*p && *p != '/') {
            if (k < sizeof(part) - 1) part[k++] = *p;
            p++;
        }
        part[k] = 0;

        if (strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) {
            if (depth > 0) { depth--; len = starts[depth]; if (len < 1) len = 1; }
            continue;
        }
        if (depth >= 32) return false;

        if (len > 1) { if (len + 1 >= cap) return false; out[len++] = '/'; }
        starts[depth++] = len - (len > 1 ? 1 : 0);
        if (len + k >= cap) return false;
        for (u32 i = 0; i < k; i++) out[len++] = part[i];
    }

    out[len] = 0;
    if (len == 0) { out[0] = '/'; out[1] = 0; }
    return true;
}

/* --- the in-memory backend ---------------------------------------------- */

/* Used only when there is no disk. Names are whole paths, and a directory is
   an entry with no contents, which is enough to list and navigate. */

static int ram_list(const char *dir, u32 index, char *name_out, u32 *size_out, bool *dir_out) {
    u32 dlen = strlen(dir);
    bool at_root = (dlen == 1);
    u32 seen = 0;

    for (u32 i = 0; i < fs_count(); i++) {
        file_t *f = fs_at(i);
        if (!f) continue;

        /* Does this entry sit directly inside `dir`? */
        if (strncmp(f->name, dir, dlen) != 0) continue;
        const char *rest = f->name + dlen;
        if (!at_root) { if (*rest != '/') continue; rest++; }
        if (!*rest) continue;
        bool nested = false;
        for (const char *p = rest; *p; p++) if (*p == '/') nested = true;
        if (nested) continue;

        if (seen == index) {
            if (name_out) { strncpy(name_out, rest, VFS_NAME_MAX - 1); name_out[VFS_NAME_MAX - 1] = 0; }
            if (size_out) *size_out = f->size;
            if (dir_out)  *dir_out = f->is_dir;
            return 1;
        }
        seen++;
    }
    return 0;
}

/* --- listing and stat --------------------------------------------------- */

/* Which volume a path is on, and what it is called there.
 *
 * Everything under /usb is the removable volume, with the prefix taken off,
 * because a volume knows nothing about where it was mounted. Everything else
 * is the disk the machine booted from.
 *
 * Deciding it here, at the edge, is what keeps the volume out of every
 * function below: they ask about the selected one, and this is the only
 * place that selects. */
#define USB_MOUNT "/usb"

static const char *route(const char *abs) {
    if (abs[0] == '/' && abs[1] == 'u' && abs[2] == 's' && abs[3] == 'b'
        && (abs[4] == '/' || abs[4] == 0)) {
        fat_select(FAT_VOL_USB);
        return abs[4] ? abs + 4 : "/";
    }
    fat_select(FAT_VOL_DISK);
    return abs;
}

int vfs_list(const char *path, u32 index, char *name_out, u32 *size_out, bool *dir_out) {
    char abs[VFS_PATH_MAX];
    if (!vfs_resolve(path ? path : ".", abs, sizeof(abs))) return -1;

    if (vfs_generated(abs)) return sysfs_list(abs, index, name_out, size_out, dir_out);

    /* The root has the live tree's two directories in it before anything
       that is actually stored, so ls shows them without them existing on
       the volume. */
    bool at_root = (abs[0] == '/' && abs[1] == 0);
    if (at_root) {
        static const char *tops[] = { "bin", "sys", "usb" };
        u32 ntop = fat_mounted_on(FAT_VOL_USB) ? 3 : 2;
        if (index < ntop) {
            if (name_out) { strncpy(name_out, tops[index], VFS_NAME_MAX - 1); name_out[VFS_NAME_MAX - 1] = 0; }
            if (size_out) *size_out = 0;
            if (dir_out)  *dir_out = true;
            return 1;
        }
        index -= ntop;
    }

    const char *on = route(abs);
    if (fat_mounted()) return fat_list(on, index, name_out, size_out, dir_out);
    return ram_list(abs, index, name_out, size_out, dir_out);
}

u32 vfs_count(const char *path) {
    u32 n = 0;
    while (vfs_list(path, n, 0, 0, 0) == 1) n++;
    return n;
}

bool vfs_stat(const char *path, u32 *size_out, bool *dir_out) {
    char abs[VFS_PATH_MAX];
    if (!vfs_resolve(path, abs, sizeof(abs))) return false;

    if (abs[0] == '/' && abs[1] == 0) {
        if (size_out) *size_out = 0;
        if (dir_out)  *dir_out = true;
        return true;
    }

    if (vfs_generated(abs)) return sysfs_stat(abs, size_out, dir_out);

    const char *on = route(abs);
    if (fat_mounted()) return fat_stat(on, size_out, dir_out);

    file_t *f = fs_find(abs);
    if (!f) return false;
    if (size_out) *size_out = f->size;
    if (dir_out)  *dir_out = f->is_dir;
    return true;
}

/* --- whole file operations ---------------------------------------------- */

int vfs_read(const char *path, void *buf, u32 cap) {
    char abs[VFS_PATH_MAX];
    if (!vfs_resolve(path, abs, sizeof(abs))) return -1;

    if (vfs_generated(abs)) return sysfs_read(abs, buf, cap);

    const char *on = route(abs);
    if (fat_mounted()) return fat_read_file(on, (u8 *)buf, cap);

    file_t *f = fs_find(abs);
    if (!f || f->is_dir) return -1;
    u32 n = f->size < cap ? f->size : cap;
    memcpy(buf, f->data, n);
    return (int)n;
}

bool vfs_write(const char *path, const void *buf, u32 len) {
    char abs[VFS_PATH_MAX];
    if (!vfs_resolve(path, abs, sizeof(abs))) return false;
    if (vfs_generated(abs)) return false;   /* generated, or the kernel's own copy */

    const char *on = route(abs);
    if (fat_mounted()) return fat_write_file(on, (const u8 *)buf, len);
    return fs_write(abs, buf, len);
}

bool vfs_append(const char *path, const void *buf, u32 len) {
    u32 size = 0;
    bool is_dir = false;
    if (!vfs_stat(path, &size, &is_dir)) return vfs_write(path, buf, len);
    if (is_dir) return false;

    u8 *joined = (u8 *)kmalloc(size + len);
    if (!joined) return false;
    bool ok = false;
    if (vfs_read(path, joined, size) >= 0) {
        memcpy(joined + size, buf, len);
        ok = vfs_write(path, joined, size + len);
    }
    kfree(joined);
    return ok;
}

bool vfs_delete(const char *path) {
    char abs[VFS_PATH_MAX];
    if (!vfs_resolve(path, abs, sizeof(abs))) return false;
    if (vfs_generated(abs)) return false;

    const char *on = route(abs);
    if (fat_mounted()) return fat_delete_file(on);
    return fs_delete(abs);
}

bool vfs_rename(const char *from, const char *to) {
    char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
    if (!vfs_resolve(from, a, sizeof(a))) return false;
    if (!vfs_resolve(to, b, sizeof(b))) return false;

    /* /bin and /sys are made up as they are read. There is nothing on a
       disk to rename and nowhere to record a new name. */
    if (vfs_generated(a) || vfs_generated(b)) return false;

    if (!fat_mounted()) return false;   /* the live tree has no rename */
    return fat_rename(route(a), route(b));
}

bool vfs_mkdir(const char *path) {
    char abs[VFS_PATH_MAX];
    if (!vfs_resolve(path, abs, sizeof(abs))) return false;
    if (abs[0] == '/' && abs[1] == 0) return false;
    if (vfs_generated(abs)) return false;

    const char *on = route(abs);
    if (fat_mounted()) return fat_mkdir(on);
    return fs_mkdir(abs);
}

bool vfs_rmdir(const char *path) {
    char abs[VFS_PATH_MAX];
    if (!vfs_resolve(path, abs, sizeof(abs))) return false;
    if (abs[0] == '/' && abs[1] == 0) return false;
    if (vfs_generated(abs)) return false;
    if (vfs_count(abs) > 0) return false;

    const char *on = route(abs);
    if (fat_mounted()) return fat_rmdir(on);
    return fs_delete(abs);
}

bool vfs_chdir(const char *path) {
    char abs[VFS_PATH_MAX];
    if (!vfs_resolve(path, abs, sizeof(abs))) return false;

    bool is_dir = false;
    if (!(abs[0] == '/' && abs[1] == 0)) {
        if (!vfs_stat(abs, 0, &is_dir) || !is_dir) return false;
    }
    set_cwd(abs);
    return true;
}

u8 *vfs_slurp(const char *path, u32 *size_out) {
    u32 size = 0;
    bool is_dir = false;
    if (!vfs_stat(path, &size, &is_dir) || is_dir) return 0;

    u8 *buf = (u8 *)kmalloc(size ? size : 1);
    if (!buf) return 0;
    int got = vfs_read(path, buf, size);
    if (got < 0) { kfree(buf); return 0; }
    if (size_out) *size_out = (u32)got;
    return buf;
}

/* Open files used to be here. They are in kernel/fd.c now, because a
   descriptor turned out to be two things rather than one: a file, and a
   number in a process that refers to it. What is left in this file is the
   part that never depended on a process at all — paths, and what is at
   them. */

void vfs_init(void) {
    sysfs_init();
}
