/* The shape of the filesystem.
 *
 * Six directories, each with one job:
 *
 *   /bin   programs. Generated: these are files in the kernel image.
 *   /sys   the machine, as files. Generated: read one and it is computed.
 *   /home  the user's own files. The shell starts here.
 *   /doc   what shipped with the system, in text.
 *   /cfg   what programs remember between runs.
 *   /tmp   scratch, emptied every boot.
 *
 * This is not a Unix layout with the names filed off. There is one user and
 * no packages, so /usr, /etc, /var and /opt would all be the same directory
 * with different names on it. What is actually different here is that two of
 * these six are not stored anywhere, and the split that matters is between
 * what the system provides and what the user made.
 *
 * The seeded files are written once. Anything edited or deleted afterwards
 * stays that way, because writing them back on every boot would quietly undo
 * somebody's work.
 */
#include "layout.h"
#include "vfs.h"
#include "fat.h"
#include "printf.h"
#include "string.h"

static const char *const DIRS[] = { "/home", "/doc", "/cfg", "/tmp" };
#define N_DIRS (sizeof(DIRS) / sizeof(DIRS[0]))

/* Bumped when a file is added below. Every file is offered exactly once, at
   the generation it was introduced, and the highest generation offered so far
   is remembered on the disk.
 *
 * Checking whether the file is there is not enough on its own: a file the
 * user deleted is also not there, and putting it back on the next boot is
 * how a system quietly undoes somebody's work. */
#define SEED_GENERATION 1
#define SEED_MARKER "/cfg/seeded"

static u32 seeded_through;

static void seed(u32 generation, const char *path, const char *text) {
    if (generation <= seeded_through) return;   /* already offered once */
    if (vfs_stat(path, 0, 0)) return;           /* or the user has their own */
    vfs_write(path, text, strlen(text));
}

static u32 read_marker(void) {
    char buf[16];
    int n = vfs_read(SEED_MARKER, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = 0;

    u32 v = 0;
    for (int i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (u32)(buf[i] - '0');
    return v;
}

static void write_marker(u32 generation) {
    char buf[16];
    int n = kformat(buf, sizeof(buf), "%d\n", generation);
    if (n > 0) vfs_write(SEED_MARKER, buf, (u32)n);
}

/* A path and everything under it. Eight levels at most: a scratch directory
   holds what programs left there, not a tree to follow without end, and each
   level costs a name and a path of stack. */
static bool remove_tree(const char *path, u32 depth) {
    bool dir = false;
    if (!vfs_stat(path, 0, &dir)) return false;
    if (!dir) return vfs_delete(path);
    if (depth >= 8) return false;

    char name[VFS_NAME_MAX];
    u32 at = 0;
    for (u32 guard = 0; guard < 1024; guard++) {
        if (vfs_list(path, at, name, 0, 0) != 1) break;
        char sub[VFS_PATH_MAX];
        kformat(sub, sizeof(sub), "%s/%s", path, name);
        /* What will not go is stepped past, so the rest still does. */
        if (!remove_tree(sub, depth + 1)) at++;
    }
    return vfs_rmdir(path);
}

/* /tmp means what it says. Files left there from last time are gone, which
   is the only guarantee that makes a scratch directory worth having.
 *
 * It looked only ever at the first entry, deleting it or removing it as an
 * empty directory, and stopped at the first that was neither: one folder
 * with something in it at the front of /tmp kept everything behind it for
 * ever. Each entry now goes as a tree, and one that will not go is stepped
 * past. */
void layout_empty_tmp(void) {
    char name[VFS_NAME_MAX];
    u32 at = 0;
    for (u32 guard = 0; guard < 1024; guard++) {
        if (vfs_list("/tmp", at, name, 0, 0) != 1) break;

        char path[VFS_PATH_MAX];
        kformat(path, sizeof(path), "/tmp/%s", name);
        if (!remove_tree(path, 0)) at++;
    }
}

/* Where the shell starts. /home, unless the disk is one another system made
   and has no such folder, in which case the root: a shell sitting in a
   directory that does not exist is a shell every relative path fails in. */
static const char *home_dir = "/home";

void layout_init(void) {
    /* On a disk another system made, nothing: no folders, no seed files and
       above all no emptying of whatever it keeps in a folder called TMP,
       which names compare to without regard to case. Its files are its own,
       and this system uses them where they are. */
    if (vfs_disk_backed() && !fat_made_here()) {
        bool dir = false;
        if (!vfs_stat("/home", 0, &dir) || !dir) home_dir = "/";
        return;
    }

    for (u32 i = 0; i < N_DIRS; i++)
        if (!vfs_stat(DIRS[i], 0, 0)) vfs_mkdir(DIRS[i]);

    layout_empty_tmp();

    seeded_through = read_marker();

    seed(1, "/doc/readme",
         "zelr\n"
         "\n"
         "An operating system written from scratch. There is no other\n"
         "system underneath this one. The machine powered on, firmware\n"
         "handed control to a bootloader in this repository, and that\n"
         "loaded the kernel that is drawing these characters.\n"
         "\n"
         "Everything here was written for it: the bootloaders, the paging\n"
         "code, the scheduler, the FAT16 driver, the TCP stack, the window\n"
         "manager, and the font on your screen.\n"
         "\n"
         "At this prompt:   guide       a walk through what works\n"
         "                   help        every command\n"
         "                   desktop     windows, a mouse and a terminal\n"
         "\n"
         "In the terminal on the desktop, which is a separate program\n"
         "running outside the kernel:\n"
         "\n"
         "                   help        its own, longer command list\n"
         "                   tree        this filesystem, all of it\n"
         "                   sys         the machine, as files\n");

    seed(1, "/doc/filesystem",
         "Where things live\n"
         "\n"
         "  /bin    programs. These are inside the kernel image, not on the\n"
         "          disk, so rebuilding the kernel replaces them.\n"
         "\n"
         "  /sys    the machine as text. None of these files are stored.\n"
         "          Reading one runs the code that works out the answer, so\n"
         "          it is never out of date.\n"
         "\n"
         "            cat /sys/memory     what is allocated\n"
         "            cat /sys/tasks      what is running\n"
         "            cat /sys/devices    what was found at boot\n"
         "            cat /sys/uptime     how long, and how busy\n"
         "            cat /sys/cpu        the processors\n"
         "            cat /sys/net        the address, if there is one\n"
         "\n"
         "  /home   your files. The shell starts here.\n"
         "  /doc    this.\n"
         "  /cfg    what programs remember. The desktop keeps its theme\n"
         "          here, which is why it looks the same next boot.\n"
         "  /tmp    scratch. Emptied every time the machine starts.\n"
         "\n"
         "/home, /doc, /cfg and /tmp are real directories on a FAT16\n"
         "volume and survive a reboot. /bin and /sys do not exist on any\n"
         "disk; they are answered by the kernel when you look.\n");

    seed(1, "/home/notes",
         "A file on a real disk.\n"
         "\n"
         "Edit it, reboot, and it will still say whatever you left here.\n");

    /* Recorded last, so a machine that loses power partway through seeding
       tries again next time rather than skipping what it never wrote. */
    if (seeded_through < SEED_GENERATION) {
        write_marker(SEED_GENERATION);
        seeded_through = SEED_GENERATION;
    }
}

const char *layout_home(void) { return home_dir; }
