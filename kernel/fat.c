/* FAT16 and FAT32, with directories.
 *
 * The point of this over a private format is interoperability: a FAT image
 * can be opened by other tools, so files move between zelr and the machine
 * hosting it. The format is old and fiddly but exhaustively documented, and
 * every field below sits where the specification says it goes, because
 * anything else produces an image other readers call corrupt.
 *
 * The awkward part of FAT16 is that the root directory is not a normal
 * directory: it lives in a fixed run of sectors before the data area and
 * cannot grow, while every other directory is an ordinary cluster chain that
 * can. Everything here goes through dir_read / dir_write, which hide that
 * difference, so the rest of the file never has to care which kind it has.
 *
 * FAT32 removes exactly that wart, and adds two of its own: table entries are
 * four bytes rather than two, and a directory entry's cluster number is split
 * across two fields sixteen bytes apart, because the high half was squeezed
 * into space FAT16 left reserved. Which of the two a volume is is not written
 * down anywhere; it is worked out from how many clusters it has, and the
 * string "FAT32" in the boot sector is a label that some formatters get
 * wrong. The count is the only answer.
 *
 * Reading and writing work on both. Formatting only produces FAT16, because
 * nothing here needs to create a FAT32 volume: the ones that matter, an EFI
 * System Partition among them, already exist and were made by something else.
 */
#include "fat.h"
#include "ata.h"
#include "blockdev.h"
#include "blackbox.h"
#include "heap.h"
#include "printf.h"
#include "string.h"

#define ATTR_READONLY  0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

#define ENT_FREE      0x00
#define ENT_DELETED   0xE5

/* The top of the range a table entry can hold, reserved to mean "the chain
   ends here". FAT32 entries are 28 bits wide, not 32: the top four are
   reserved and must be left as they are found. */
#define EOC16_MIN 0x0000FFF8u
#define EOC16     0x0000FFFFu
#define EOC32_MIN 0x0FFFFFF8u
#define EOC32     0x0FFFFFFFu

typedef struct {
    u8  name[11];
    u8  attr;
    u8  nt_reserved;
    u8  create_tenth;
    u16 create_time, create_date;
    u16 access_date;
    u16 cluster_hi;          /* always zero on FAT16 */
    u16 write_time, write_date;
    u16 cluster_lo;
    u32 size;
} __attribute__((packed)) dirent_t;

/* Where a directory lives. The root has no cluster chain of its own. */
typedef struct {
    bool root;
    u32  cluster;
} dir_t;

/* Where this volume begins on the disk. Zero for an image written without a
   partition table, which is what this kernel formats and what QEMU is given;
   anything else on a real disk. Every sector number below is relative to it,
   so the arithmetic in the rest of this file did not have to change. */
/* Everything that is true of one volume rather than of the filesystem code.
 *
 * This was two dozen file scope variables, which is the same thing written
 * down in a way that only allows one of them. They are fields now and the
 * names below are macros onto whichever volume is selected, so the thousand
 * lines underneath did not have to change and could not have been missed.
 *
 * There are two: the disk the machine booted from, and something removable.
 * That is enough to copy a file from one to the other, which is the entire
 * point of the exercise. */
typedef struct {
    u32  dev;                           /* which disk, for blk_read_on */
    u32  part_base;
    u32  part_sectors;                  /* 0 means to the end of the disk */

    u32  fat_bits;                      /* 16 or 32, from the cluster count */
    u32  root_cluster;                  /* FAT32 only: the root is a chain */

    bool mounted;
    u16  bytes_per_sector;
    u8   sectors_per_cluster;
    u16  reserved_sectors;
    u8   num_fats;
    u16  root_entries;
    u32  total_sectors;
    u32  fat_sectors;
    u32  fat_start;
    u32  root_start;
    u32  root_sectors;
    u32  data_start;
    u32  cluster_count;

    u32  alloc_hint;
    u32  fat_cache_lba;
    bool fat_cache_valid;
    u8   fat_cache[SECTOR_SIZE];
} fatvol_t;

static fatvol_t volumes[FAT_VOLUMES] = {
    { .fat_bits = 16, .alloc_hint = 2 },
    { .fat_bits = 16, .alloc_hint = 2 },
};

/* Which one the calls below are about. Set at the edge, in kernel/vfs.c,
   from the path: everything under /usb is the removable one and everything
   else is the disk the machine booted from. */
static u32 current_volume = FAT_VOL_DISK;

void fat_select(u32 vol) {
    if (vol < FAT_VOLUMES) current_volume = vol;
}

u32 fat_selected(void) { return current_volume; }

bool fat_mounted_on(u32 vol) {
    return vol < FAT_VOLUMES && volumes[vol].mounted;
}

void fat_forget_volume(u32 vol) {
    if (vol >= FAT_VOLUMES) return;
    volumes[vol].mounted = false;
    volumes[vol].fat_cache_valid = false;
    volumes[vol].fat_bits = 16;
    volumes[vol].alloc_hint = 2;
}

#define CUR                 volumes[current_volume]
#define part_base           (CUR.part_base)
#define part_sectors        (CUR.part_sectors)
#define fat_bits            (CUR.fat_bits)
#define root_cluster        (CUR.root_cluster)
#define mounted             (CUR.mounted)
#define bytes_per_sector    (CUR.bytes_per_sector)
#define sectors_per_cluster (CUR.sectors_per_cluster)
#define reserved_sectors    (CUR.reserved_sectors)
#define num_fats            (CUR.num_fats)
#define root_entries        (CUR.root_entries)
#define total_sectors       (CUR.total_sectors)
#define fat_sectors         (CUR.fat_sectors)
#define fat_start           (CUR.fat_start)
#define root_start          (CUR.root_start)
#define root_sectors        (CUR.root_sectors)
#define data_start          (CUR.data_start)
#define cluster_count       (CUR.cluster_count)
#define alloc_hint          (CUR.alloc_hint)
#define fat_cache           (CUR.fat_cache)
#define fat_cache_lba       (CUR.fat_cache_lba)
#define fat_cache_valid     (CUR.fat_cache_valid)



static bool vol_read(u32 lba, u32 count, void *buf) {
    if (part_sectors && (lba >= part_sectors || count > part_sectors - lba))
        return false;
    return blk_read_on(CUR.dev, part_base + lba, count, buf);
}

static bool vol_write(u32 lba, u32 count, const void *buf) {
    if (part_sectors && (lba >= part_sectors || count > part_sectors - lba))
        return false;
    return blk_write_on(CUR.dev, part_base + lba, count, buf);
}

u32 fat_base(void) { return part_base; }

/* 16 or 32. Decided by the cluster count at mount, never by the label. */

static u32 eoc_min(void) { return fat_bits == 32 ? EOC32_MIN : EOC16_MIN; }
static u32 eoc(void)     { return fat_bits == 32 ? EOC32     : EOC16; }


/* Three buffers, because there are three things being read at once and any
   two of them sharing one would overwrite each other. A directory operation
   consults the allocation table part way through, and following a chain
   happens while a caller is part way through a data sector. */
static u8 sec[SECTOR_SIZE];      /* file data and the boot sector */
static u8 dsec[SECTOR_SIZE];     /* directory entries */

/* One sector of the table, held in memory.
 *
 * Finding a free cluster walks the table, and 256 entries share a sector.
 * Reading that sector once per entry instead of once per 256 is what made
 * writing a file cost time proportional to the size of the whole volume. */

/* Where the last search stopped. A file is a run of allocations, and each
   one restarting at the front of the table is what made it quadratic. */

static void fat_forget(void) {
    fat_cache_valid = false;
    alloc_hint = 2;
}

static bool fat_cache_load(u32 lba) {
    if (fat_cache_valid && fat_cache_lba == lba) return true;
    if (!vol_read(lba, 1, fat_cache)) { fat_cache_valid = false; return false; }
    fat_cache_lba = lba;
    fat_cache_valid = true;
    return true;
}

bool fat_mounted(void) { return mounted; }
u32  fat_total_clusters(void) { return cluster_count; }
u32  fat_cluster_bytes(void) { return (u32)sectors_per_cluster * SECTOR_SIZE; }

static const dir_t ROOT = { true, 0 };

/* --- 8.3 names ---------------------------------------------------------- */

static char upcase(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* "readme.txt" becomes "README  TXT". Anything that will not fit is
   truncated rather than rejected, which matches what DOS did. */
static void to_83(const char *name, u8 out[11]) {
    memset(out, ' ', 11);
    u32 i = 0;
    while (name[i] && name[i] != '.' && i < 8) { out[i] = (u8)upcase(name[i]); i++; }
    const char *dot = name;
    while (*dot && *dot != '.') dot++;
    if (*dot == '.') {
        dot++;
        for (u32 j = 0; j < 3 && dot[j]; j++) out[8 + j] = (u8)upcase(dot[j]);
    }
}

static char downcase(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* FAT stores names folded to upper case. Handing "README.TXT" back to a
   case sensitive filesystem would make it a different file from the
   "readme.txt" that created it, so fold the other way on the way out. */
static void from_83(const u8 in[11], char *out) {
    u32 n = 0;
    for (u32 i = 0; i < 8 && in[i] != ' '; i++) out[n++] = downcase((char)in[i]);
    if (in[8] != ' ') {
        out[n++] = '.';
        for (u32 i = 8; i < 11 && in[i] != ' '; i++) out[n++] = downcase((char)in[i]);
    }
    out[n] = 0;
}

/* --- the file allocation table ------------------------------------------ */

static u32 fat_get(u32 cluster) {
    u32 width = fat_bits / 8;
    u32 off = cluster * width;
    if (!fat_cache_load(fat_start + off / SECTOR_SIZE)) return eoc();
    if (fat_bits == 32)
        return *(u32 *)(fat_cache + (off % SECTOR_SIZE)) & 0x0FFFFFFFu;
    return *(u16 *)(fat_cache + (off % SECTOR_SIZE));
}

static bool fat_set(u32 cluster, u32 value) {
    u32 width = fat_bits / 8;
    u32 off = cluster * width;
    u32 lba = fat_start + off / SECTOR_SIZE;
    if (!fat_cache_load(lba)) return false;
    if (fat_bits == 32) {
        /* The top four bits belong to whoever set them and are not ours to
           change, so the new value goes in underneath them. */
        u32 *slot = (u32 *)(fat_cache + (off % SECTOR_SIZE));
        *slot = (*slot & 0xF0000000u) | (value & 0x0FFFFFFFu);
    } else {
        *(u16 *)(fat_cache + (off % SECTOR_SIZE)) = (u16)value;
    }

    /* Written through rather than buffered: fat_write_file's crash safety
       depends on the new chain really reaching the disk before the directory
       entry that points at it. Both copies of the table have to agree or
       other readers will object, and since they are byte for byte the same,
       the one sector goes to each of them. */
    for (u32 copy = 0; copy < num_fats; copy++) {
        if (!vol_write(lba + copy * fat_sectors, 1, fat_cache)) {
            fat_cache_valid = false;
            return false;
        }
    }
    return true;
}

static u32 alloc_cluster(void) {
    if (!cluster_count) return 0;
    /* Resume where the last search stopped, wrapping once round the table. */
    for (u32 n = 0; n < cluster_count; n++) {
        u32 c = 2 + (alloc_hint - 2 + n) % cluster_count;
        if (fat_get(c) != 0) continue;
        if (!fat_set(c, eoc())) return 0;
        alloc_hint = (c + 1 < cluster_count + 2) ? c + 1 : 2;
        return c;
    }
    return 0;
}

static void free_chain(u32 cluster) {
    while (cluster >= 2 && cluster < eoc_min()) {
        u32 next = fat_get(cluster);
        fat_set(cluster, 0);
        /* Somewhere behind the hint is free again, so look there next. */
        if (cluster < alloc_hint) alloc_hint = cluster;
        cluster = next;
    }
}

static u32 cluster_lba(u32 cluster) {
    return data_start + (cluster - 2) * sectors_per_cluster;
}

static u32 zero_cluster(u32 cluster) {
    memset(sec, 0, SECTOR_SIZE);
    u32 lba = cluster_lba(cluster);
    for (u32 s = 0; s < sectors_per_cluster; s++)
        if (!vol_write(lba + s, 1, sec)) return 0;
    return cluster;
}

/* --- mounting ----------------------------------------------------------- */

/* Mounts a volume of a particular disk. The selection is put back
   afterwards, so mounting something does not change what the next call is
   about. */
bool fat_mount_on(u32 vol, u32 dev, u32 base_lba) {
    if (vol >= FAT_VOLUMES) return false;
    u32 was = current_volume;
    current_volume = vol;
    volumes[vol].dev = dev;
    bool ok = fat_mount_at(base_lba);
    current_volume = was;
    return ok;
}

bool fat_mount_at(u32 base_lba) {
    mounted = false;
    fat_forget();
    if (!blk_device_present(CUR.dev)) return false;

    part_base = base_lba;
    part_sectors = 0;                   /* not known until the volume says */
    if (!vol_read(0, 1, sec)) return false;

    if (sec[510] != 0x55 || sec[511] != 0xAA) return false;

    bytes_per_sector    = *(u16 *)(sec + 11);
    sectors_per_cluster = sec[13];
    reserved_sectors    = *(u16 *)(sec + 14);
    num_fats            = sec[16];
    root_entries        = *(u16 *)(sec + 17);
    u16 total16         = *(u16 *)(sec + 19);
    u16 fat_sectors16   = *(u16 *)(sec + 22);
    u32 total32         = *(u32 *)(sec + 32);

    /* A zero in the sixteen bit table size is what says to look in the
       thirty two bit one, which is a field FAT16 does not have and FAT32
       put in space that used to be reserved. */
    fat_sectors = fat_sectors16 ? (u32)fat_sectors16 : *(u32 *)(sec + 36);

    total_sectors = total16 ? total16 : total32;

    if (bytes_per_sector != SECTOR_SIZE) return false;
    if (sectors_per_cluster == 0 || num_fats == 0 || fat_sectors == 0) return false;

    fat_start    = reserved_sectors;
    root_sectors = ((u32)root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    root_start   = fat_start + (u32)num_fats * fat_sectors;
    data_start   = root_start + root_sectors;

    if (data_start >= total_sectors) return false;
    cluster_count = (total_sectors - data_start) / sectors_per_cluster;

    /* Which of the two this is, decided the only way the specification
       allows: by counting clusters. The "FAT32" written at offset 82 is a
       label, some formatters get it wrong, and nothing is required to read
       it. Below 4085 the volume is FAT12, which this does not do. */
    if (cluster_count < 4085) return false;
    fat_bits = (cluster_count > 65524) ? 32 : 16;

    if (fat_bits == 32) {
        /* The root has no fixed home; it is a chain like any other
           directory, so there are no root sectors between the tables and the
           data and the data starts earlier than the arithmetic above put it.
           Recomputing the cluster count matters: getting it wrong here is
           what turns a readable volume into one that looks corrupt. */
        if (root_entries != 0) return false;
        root_sectors = 0;
        root_start   = 0;
        data_start   = fat_start + (u32)num_fats * fat_sectors;
        if (data_start >= total_sectors) return false;
        cluster_count = (total_sectors - data_start) / sectors_per_cluster;

        root_cluster = *(u32 *)(sec + 44);
        if (root_cluster < 2 || root_cluster >= cluster_count + 2) return false;
    } else {
        root_cluster = 0;
        if (root_entries == 0) return false;
    }

    /* Now that the volume has said how big it claims to be, hold it to it.
       A volume whose total_sectors runs past the end of its partition is
       either corrupt or someone else's, and either way the rest of this
       file must not be allowed to read outside it. */
    if (total_sectors == 0) return false;
    part_sectors = total_sectors;

    u32 disk = blk_sectors();
    if (disk && (part_base >= disk || total_sectors > disk - part_base)) {
        part_sectors = 0;
        return false;
    }

    mounted = true;
    return true;
}

bool fat_mount(void) { return fat_mount_at(0); }

u32 fat_type(void) { return mounted ? fat_bits : 0; }

/* The serial this kernel stamps on a volume it made. It is the ASCII "ZLR"
   with a zero under it, which is not a number anything else would land on
   by accident. */
#define ZELR_VOLUME_ID 0x5A4C5200u

/* The two fields fat_format writes and nothing else does. Checked against
   the boot sector rather than remembered from the mount, so it is still
   right if something else rewrote the volume underneath us. */
bool fat_is_zelr_volume(void) {
    if (!mounted) return false;
    u8 boot[SECTOR_SIZE];
    if (!vol_read(0, 1, boot)) return false;
    return fat_boot_is_ours(boot);
}

/* Whether a boot sector is one this kernel wrote, and where in its reserved
   area the log goes. Both are asked of a sector that has been read but not
   mounted, because the log is read before anything is decided and written
   from the fault path.

   The volume serial number is the marker, because it is the one field a
   formatter is free to put anything in. It lives at a different offset on
   the two widths: FAT32 put four bytes of table size where FAT16 keeps the
   serial, and moved the serial down past the fields that came with it. */
static bool boot_is_fat32(const u8 *boot) {
    return *(const u16 *)(boot + 17) == 0 && *(const u16 *)(boot + 22) == 0;
}

bool fat_boot_is_ours(const u8 *boot) {
    if (boot[510] != 0x55 || boot[511] != 0xAA) return false;
    if (memcmp(boot + 3, "ZELR    ", 8) != 0) return false;
    u32 at = boot_is_fat32(boot) ? 67 : 39;
    return *(const u32 *)(boot + at) == ZELR_VOLUME_ID;
}

/* FAT16 has nothing in its reserved area but the boot sector, so the log
   starts straight after it. FAT32 has three more things there that other
   readers expect at the addresses the specification names -- the filesystem
   information sector, and a backup of the boot sector and of that -- so the
   log starts after the last of them. */
u32 fat_boot_log_lba(const u8 *boot) { return boot_is_fat32(boot) ? 8 : 1; }

u32 fat_boot_reserved(const u8 *boot) { return *(const u16 *)(boot + 14); }

/* How many sectors to a cluster, so that the number of clusters lands in the
   range the width can describe.
 *
 * FAT16 counts clusters in sixteen bits and reserves the top of the range,
 * which leaves 65524 of them. At the two kilobyte clusters this used to
 * always use, that is 128 MiB, and everything past it was refused: an eight
 * gigabyte disk asked for a table with four million entries in it, the
 * count came out above what the width holds, and formatting failed with
 * nothing to do about it. Which is what "could not prepare the disk" was.
 *
 * So the cluster grows with the volume until thirty two kilobytes, the
 * largest FAT16 is ordinarily written with, and past that -- about two
 * gigabytes -- the volume is FAT32, which this can now write. */
static bool fat_layout(u32 total, u32 spc, u32 bits, u32 reserved,
                       u32 fats, u32 roots, u32 *fsize_out, u32 *clusters_out) {
    u32 ent = (bits == 32) ? 4 : 2;
    u32 root_secs = ((u32)roots * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;

    /* The table has to be big enough to describe the clusters that are left
       after the table itself is subtracted, so solve for it. */
    u32 fsize = 1, clusters = 0, data = 0;
    for (int i = 0; i < 16; i++) {
        if (total <= reserved + fats * fsize + root_secs) return false;
        data = total - reserved - fats * fsize - root_secs;
        clusters = data / spc;
        u32 need = ((clusters + 2) * ent + SECTOR_SIZE - 1) / SECTOR_SIZE;
        if (need == fsize) break;
        fsize = need;
    }

    /* And if it did not settle, err upwards: a table with room for clusters
       that do not exist is wasted space, one with room for fewer than there
       are is a volume that reads its own data as somebody else's. */
    if (total <= reserved + fats * fsize + root_secs) return false;
    data = total - reserved - fats * fsize - root_secs;
    clusters = data / spc;
    u32 need = ((clusters + 2) * ent + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (need > fsize) {
        fsize = need;
        if (total <= reserved + fats * fsize + root_secs) return false;
        data = total - reserved - fats * fsize - root_secs;
        clusters = data / spc;
    }

    *fsize_out = fsize;
    *clusters_out = clusters;
    return true;
}

/* --- something that runs ---------------------------------------------------
 *
 * A boot sector ends with 0x55 0xAA, and that is not decoration. It is the
 * mark a BIOS looks for to decide a disk can be started from; finding it,
 * the firmware loads the sector to 0x7C00 and jumps to it.
 *
 * This wrote the mark and left the sector otherwise empty. The jump at the
 * front went to the first byte after the parameter block, and the first
 * byte after the parameter block was nought, and so was everything after
 * that: 0x00 0x00 decodes as `add [bx+si], al`, so the processor walked
 * through four hundred bytes of it and then off the end of the sector into
 * whatever was next. On a machine whose disk this is, that is the whole of
 * the second boot -- the first one formats the disk, and every one after it
 * finds a disk the firmware believes in and cannot run. VMware reports it
 * as "tried to execute an invalid part of memory", which is exactly what
 * happened and gives no hint of where it came from.
 *
 * So the sector carries a program now. It is the same one every formatter
 * writes: say the disk is not the one to start from, and stop. Sixteen bit
 * real mode, because that is what the processor is in when this runs.
 */
static void fat_boot_stub(u8 *sec, u32 at) {
    static const u8 code[] = {
        0xFA,                   /* cli                  */
        0x31, 0xC0,             /* xor ax, ax           */
        0x8E, 0xD8,             /* mov ds, ax           */
        0xBE, 0x00, 0x00,       /* mov si, message      (filled in below) */
        0xAC,                   /* next: lodsb          */
        0x84, 0xC0,             /* test al, al          */
        0x74, 0x09,             /* jz stop              */
        0xB4, 0x0E,             /* mov ah, 0x0E         teletype output */
        0xBB, 0x07, 0x00,       /* mov bx, 0x0007       page 0, grey      */
        0xCD, 0x10,             /* int 0x10             */
        0xEB, 0xF2,             /* jmp next             */
        0xF4,                   /* stop: hlt            */
        0xEB, 0xFD              /* jmp stop             */
    };
    static const char words[] =
        "This disk holds files. Start the machine from its installer "
        "instead.\r\n";

    u32 n = (u32)sizeof(code);
    u32 at_words = at + n;
    if (at_words + sizeof(words) > 510) return;      /* no room: leave it */

    memcpy(sec + at, code, n);
    /* Where the message is once the firmware has put the sector at 0x7C00. */
    sec[at + 6] = (u8)((0x7C00u + at_words) & 0xFF);
    sec[at + 7] = (u8)((0x7C00u + at_words) >> 8);
    memcpy(sec + at_words, words, sizeof(words));
}

/* The same sector, on a disk that was already made wrong.
 *
 * Shipping the fix above does not repair anything, because a disk that
 * mounts is never formatted again: a machine upgraded to a kernel that
 * writes a good sector still starts from the bad one it wrote last time,
 * and the only cure on offer would be erasing every file on it to get a
 * fresh one. That is not a fix, it is the same fault with a worse remedy.
 *
 * So it is repaired where it lies. What changes is the jump at the front
 * and the empty space behind the parameter block; the parameters
 * themselves, the tables, and every file the volume holds are not read
 * here and not written. Three things have to hold before a byte moves:
 *
 *   the volume is one this kernel wrote, which the serial says and which
 *   nothing else lands on, so somebody's Windows stick is never touched;
 *   the sector claims to be startable, since a sector with no mark is not
 *   making the promise this is about; and the jump lands on nought, which
 *   is the difference between empty space and somebody else's program.
 *
 * The last of those is also what makes this safe to run on every mount:
 * a sector already carrying the stub fails it and is left alone.
 */
bool fat_boot_repair(void) {
    if (!mounted) return false;

    u8 boot[SECTOR_SIZE];
    if (!vol_read(0, 1, boot)) return false;
    if (!fat_boot_is_ours(boot)) return false;

    u32 code_at = boot_is_fat32(boot) ? 0x5A : 0x3E;
    if (boot[code_at] != 0x00) return false;       /* something runs there */

    boot[0] = 0xEB; boot[1] = (u8)(code_at - 2); boot[2] = 0x90;
    fat_boot_stub(boot, code_at);
    if (boot[code_at] == 0x00) return false;       /* no room; leave it be */

    if (!vol_write(0, 1, boot)) return false;

    /* FAT32 keeps a second copy of the sector for a reader that finds the
       first one unreadable. A copy of the fault is still the fault, so it
       goes too -- and only where this volume says it put it, inside the
       reserved area, which is the one place it can be. */
    if (boot_is_fat32(boot)) {
        u32 spare = *(u16 *)(boot + 50);
        if (spare && spare < reserved_sectors) vol_write(spare, 1, boot);
    }
    return true;
}

bool fat_format_at(u32 base_lba, u32 sectors, const char *label) {
    if (!blk_present()) return false;
    fat_forget();

    part_base = base_lba;
    part_sectors = sectors;

    u32 total = sectors ? sectors : blk_sectors() - base_lba;
    if (total < 8192) return false;

    u8  fats = 2;
    u32 spc = 0, fsize = 0, clusters = 0;
    u32 reserved = 0, roots = 0;
    u32 bits = 0;

    /* FAT16 for anything it can describe, because it is the simpler volume
       and every small disk and every image made before this was one. */
    for (u32 try_spc = 4; try_spc <= 64 && !bits; try_spc *= 2) {
        u32 f = 0, c = 0;
        u32 res = 1 + BB_SECTORS;
        if (!fat_layout(total, try_spc, 16, res, fats, 512, &f, &c)) continue;
        if (c < 4085 || c > 65524) continue;
        bits = 16; spc = try_spc; fsize = f; clusters = c;
        reserved = res; roots = 512;
    }

    /* And FAT32 past that. The cluster sizes are the ones every other
       formatter uses for a disk of each size, so a volume made here looks
       ordinary to whatever reads it next. */
    if (!bits) {
        u32 try_spc = total <= 16777216u ? 8
                    : total <= 33554432u ? 16
                    : total <= 67108864u ? 32 : 64;
        for (; try_spc <= 128 && !bits; try_spc *= 2) {
            u32 f = 0, c = 0;
            /* The boot sector, the information sector, a backup of both at
               six and seven, then the log. */
            u32 res = 8 + BB_SECTORS;
            if (!fat_layout(total, try_spc, 32, res, fats, 0, &f, &c)) continue;
            if (c < 65525 || c > 0x0FFFFFF5u) continue;
            bits = 32; spc = try_spc; fsize = f; clusters = c;
            reserved = res; roots = 0;
        }
    }

    if (!bits) return false;

    fat_bits = bits;
    root_cluster = (bits == 32) ? 2 : 0;

    u32 root_secs = ((u32)roots * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;

    /* boot sector */
    memset(sec, 0, SECTOR_SIZE);
    /* FAT32 keeps another twenty six bytes of parameters after the ones
       FAT16 has, so its code starts later and its jump has to say so.
       Both widths were told 0x3C, which on a FAT32 volume is a jump into
       the middle of the volume label. */
    u32 code_at = (bits == 32) ? 0x5A : 0x3E;
    sec[0] = 0xEB;
    sec[1] = (u8)(code_at - 2);
    sec[2] = 0x90;
    memcpy(sec + 3, "ZELR    ", 8);
    *(u16 *)(sec + 11) = SECTOR_SIZE;
    sec[13] = (u8)spc;
    *(u16 *)(sec + 14) = (u16)reserved;
    sec[16] = fats;
    *(u16 *)(sec + 17) = (u16)roots;
    *(u16 *)(sec + 19) = 0;
    sec[21] = 0xF8;                     /* fixed disk */
    *(u16 *)(sec + 22) = (bits == 32) ? 0 : (u16)fsize;
    *(u16 *)(sec + 24) = 32;
    *(u16 *)(sec + 26) = 8;
    *(u32 *)(sec + 28) = 0;
    *(u32 *)(sec + 32) = total;

    if (bits == 32) {
        *(u32 *)(sec + 36) = fsize;     /* the table size lives here instead */
        *(u16 *)(sec + 40) = 0;         /* both tables, kept in step */
        *(u16 *)(sec + 42) = 0;         /* version zero, the only one */
        *(u32 *)(sec + 44) = 2;         /* the root is an ordinary chain */
        *(u16 *)(sec + 48) = 1;         /* where the free count is kept */
        *(u16 *)(sec + 50) = 6;         /* and where the spare copy goes */
        sec[64] = 0x80;
        sec[66] = 0x29;                 /* extended boot signature */
        *(u32 *)(sec + 67) = ZELR_VOLUME_ID;
        memset(sec + 71, ' ', 11);
        for (u32 i = 0; i < 11 && label && label[i]; i++)
            sec[71 + i] = (u8)upcase(label[i]);
        memcpy(sec + 82, "FAT32   ", 8);
    } else {
        sec[36] = 0x80;
        sec[38] = 0x29;                 /* extended boot signature */
        *(u32 *)(sec + 39) = ZELR_VOLUME_ID;
        memset(sec + 43, ' ', 11);
        for (u32 i = 0; i < 11 && label && label[i]; i++)
            sec[43 + i] = (u8)upcase(label[i]);
        memcpy(sec + 54, "FAT16   ", 8);
    }
    fat_boot_stub(sec, code_at);
    sec[510] = 0x55; sec[511] = 0xAA;

    u8 boot[SECTOR_SIZE];
    memcpy(boot, sec, SECTOR_SIZE);
    if (!vol_write(0, 1, sec)) return false;

    /* The reserved sectors past the boot sector, cleared. They are where the
       black box writes, and it will not write over anything it does not
       recognise; leaving a previous volume's log there would either be read
       back as this volume's own history or block the log entirely. */
    memset(sec, 0, SECTOR_SIZE);
    for (u32 s = 1; s < reserved; s++)
        if (!vol_write(s, 1, sec)) return false;

    if (bits == 32) {
        /* The information sector: a count of free clusters and a hint at
           where to start looking, which nothing here reads and every other
           reader expects to find. Both are written as unknown rather than
           as a number that will be wrong the moment a file is made. */
        memset(sec, 0, SECTOR_SIZE);
        *(u32 *)(sec + 0)   = 0x41615252u;
        *(u32 *)(sec + 484) = 0x61417272u;
        *(u32 *)(sec + 488) = 0xFFFFFFFFu;
        *(u32 *)(sec + 492) = 0xFFFFFFFFu;
        *(u32 *)(sec + 508) = 0xAA550000u;
        if (!vol_write(1, 1, sec)) return false;
        if (!vol_write(7, 1, sec)) return false;
        if (!vol_write(6, 1, boot)) return false;
    }

    /* both tables, cleared, with the reserved entries at the front */
    memset(sec, 0, SECTOR_SIZE);
    for (u32 copy = 0; copy < fats; copy++)
        for (u32 s = 0; s < fsize; s++)
            if (!vol_write(reserved + copy * fsize + s, 1, sec)) return false;

    memset(sec, 0, SECTOR_SIZE);
    if (bits == 32) {
        *(u32 *)(sec + 0) = 0x0FFFFFF8u;    /* media descriptor copy */
        *(u32 *)(sec + 4) = 0x0FFFFFFFu;    /* end of chain marker */
        *(u32 *)(sec + 8) = 0x0FFFFFFFu;    /* and the root, one cluster long */
    } else {
        *(u16 *)(sec + 0) = 0xFFF8;
        *(u16 *)(sec + 2) = 0xFFFF;
    }
    for (u32 copy = 0; copy < fats; copy++)
        if (!vol_write(reserved + copy * fsize, 1, sec)) return false;

    /* empty root directory: a fixed run of sectors on FAT16, one cluster of
       the data area on FAT32 */
    memset(sec, 0, SECTOR_SIZE);
    u32 root_lba = reserved + (u32)fats * fsize;
    u32 root_len = (bits == 32) ? spc : root_secs;
    for (u32 s = 0; s < root_len; s++)
        if (!vol_write(root_lba + s, 1, sec)) return false;

    blk_flush();
    (void)clusters;
    return fat_mount_at(base_lba);
}

bool fat_format(const char *label) { return fat_format_at(0, 0, label); }

/* --- directories -------------------------------------------------------- */

static u32 entries_per_cluster(void) { return fat_cluster_bytes() / 32; }

/* How many entries this directory can currently hold. A subdirectory grows,
   so this is a snapshot rather than a fixed number. */
static u32 dir_capacity(const dir_t *d) {
    if (d->root && fat_bits != 32) return root_entries;
    u32 n = 0, c = d->root ? root_cluster : d->cluster, guard = 0;
    while (c >= 2 && c < eoc_min() && guard++ < cluster_count + 2) {
        n += entries_per_cluster();
        c = fat_get(c);
    }
    return n;
}

/* The sector holding entry `index`, and its offset within it. */
/* Where a directory entry's cluster number lives.
 *
 * FAT16 keeps it in one sixteen bit field and leaves another reserved. FAT32
 * uses the reserved one for the high half, sixteen bytes earlier in the
 * entry. Reading only the low half on a FAT32 volume works perfectly until a
 * file lands above cluster 65535, at which point it silently points at a
 * different file's data. */
static u32 ent_cluster(const dirent_t *e) {
    if (fat_bits == 32)
        return ((u32)e->cluster_hi << 16) | e->cluster_lo;
    return e->cluster_lo;
}

static void set_ent_cluster(dirent_t *e, u32 cluster) {
    e->cluster_lo = (u16)(cluster & 0xFFFF);
    e->cluster_hi = (fat_bits == 32) ? (u16)(cluster >> 16) : 0;
}

static bool dir_locate(const dir_t *d, u32 index, u32 *lba_out, u32 *off_out) {
    u32 per_sector = SECTOR_SIZE / 32;

    /* On FAT16 the root is a fixed run of sectors that cannot grow. On FAT32
       it is an ordinary chain like any other directory, so the only thing
       the root flag still means there is "start from root_cluster". */
    if (d->root && fat_bits != 32) {
        if (index >= root_entries) return false;
        *lba_out = root_start + index / per_sector;
        *off_out = (index % per_sector) * 32;
        return true;
    }

    u32 per_cluster = entries_per_cluster();
    u32 want = index / per_cluster;
    u32 c = d->root ? root_cluster : d->cluster;
    u32 guard = 0;
    while (want-- > 0) {
        if (c < 2 || c >= eoc_min()) return false;
        c = fat_get(c);
        if (guard++ > cluster_count + 2) return false;
    }
    if (c < 2 || c >= eoc_min()) return false;

    u32 within = index % per_cluster;
    *lba_out = cluster_lba(c) + within / per_sector;
    *off_out = (within % per_sector) * 32;
    return true;
}

static bool dir_read(const dir_t *d, u32 index, dirent_t *out) {
    u32 lba, off;
    if (!dir_locate(d, index, &lba, &off)) return false;
    if (!vol_read(lba, 1, dsec)) return false;
    memcpy(out, dsec + off, 32);
    return true;
}

static bool dir_write(const dir_t *d, u32 index, const dirent_t *in) {
    u32 lba, off;
    if (!dir_locate(d, index, &lba, &off)) return false;
    if (!vol_read(lba, 1, dsec)) return false;
    memcpy(dsec + off, in, 32);
    return vol_write(lba, 1, dsec);
}

/* Adds one cluster to a directory and zeroes it, so the new entries read as
   free. On FAT16 the root cannot grow, which is the format rather than an
   omission; on FAT32 it is an ordinary chain and grows like any other. */
static bool dir_grow(const dir_t *d) {
    if (d->root && fat_bits != 32) return false;

    u32 last = d->root ? root_cluster : d->cluster;
    u32 guard = 0;
    while (guard++ < cluster_count + 2) {
        u32 next = fat_get(last);
        if (next >= eoc_min()) break;
        if (next < 2) return false;
        last = next;
    }

    u32 c = alloc_cluster();
    if (!c) return false;
    if (!zero_cluster(c)) { fat_set(c, 0); return false; }
    return fat_set(last, c);
}

/* Finds a usable slot, growing the directory if it is full. */
static int dir_free_slot(const dir_t *d) {
    for (int attempt = 0; attempt < 2; attempt++) {
        u32 cap = dir_capacity(d);
        dirent_t e;
        for (u32 i = 0; i < cap; i++) {
            if (!dir_read(d, i, &e)) break;
            if (e.name[0] == ENT_FREE || e.name[0] == ENT_DELETED) return (int)i;
        }
        /* Full. Grow once and look again; if that does not help, or this is
           the root, which cannot grow, there is nowhere to put it. */
        if (attempt > 0 || !dir_grow(d)) return -1;
    }
    return -1;
}

/* True for entries that describe a real file or directory. */
static bool entry_is_real(const dirent_t *e) {
    if (e->name[0] == ENT_FREE || e->name[0] == ENT_DELETED) return false;
    if ((e->attr & ATTR_LFN) == ATTR_LFN) return false;
    if (e->attr & ATTR_VOLUME_ID) return false;
    return true;
}

/* --- long names ---------------------------------------------------------
 *
 * A FAT directory entry holds eight characters and three more, folded to
 * upper case, and that is all the 1981 specification allows. Everything since
 * has carried the real name in extra entries placed in front of the short
 * one, each holding thirteen characters, numbered backwards, and marked with
 * an attribute combination that a reader from 1981 skips as a volume label.
 *
 * Each of those entries also carries a checksum of the short name it belongs
 * to. That is what makes the arrangement safe: a disk edited by something
 * that only understands short names leaves the long entries behind pointing
 * at a name that has changed, the checksum no longer matches, and a reader
 * that checks it falls back to the short name instead of showing a file that
 * is not there.
 *
 * Short names are still written for anything that fits in one, so every file
 * this system wrote before today reads back exactly as it did.
 */
#define ATTR_LFN_MASK 0x3F
#define LFN_CHARS     13
#define LFN_LAST      0x40

typedef struct {
    u8  seq;
    u8  part1[10];            /* five characters */
    u8  attr;                 /* always ATTR_LFN */
    u8  type;
    u8  checksum;             /* of the short name this belongs to */
    u8  part2[12];            /* six more */
    u16 cluster;              /* always zero */
    u8  part3[4];             /* and the last two */
} __attribute__((packed)) lfn_t;

static u8 short_checksum(const u8 name[11]) {
    u8 sum = 0;
    for (u32 i = 0; i < 11; i++)
        sum = (u8)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

static void lfn_get(const lfn_t *l, u16 out[LFN_CHARS]) {
    for (u32 i = 0; i < 5; i++)
        out[i] = (u16)(l->part1[i * 2] | (l->part1[i * 2 + 1] << 8));
    for (u32 i = 0; i < 6; i++)
        out[5 + i] = (u16)(l->part2[i * 2] | (l->part2[i * 2 + 1] << 8));
    for (u32 i = 0; i < 2; i++)
        out[11 + i] = (u16)(l->part3[i * 2] | (l->part3[i * 2 + 1] << 8));
}

static void lfn_put(lfn_t *l, const u16 in[LFN_CHARS]) {
    for (u32 i = 0; i < 5; i++) {
        l->part1[i * 2] = (u8)in[i];
        l->part1[i * 2 + 1] = (u8)(in[i] >> 8);
    }
    for (u32 i = 0; i < 6; i++) {
        l->part2[i * 2] = (u8)in[5 + i];
        l->part2[i * 2 + 1] = (u8)(in[5 + i] >> 8);
    }
    for (u32 i = 0; i < 2; i++) {
        l->part3[i * 2] = (u8)in[11 + i];
        l->part3[i * 2 + 1] = (u8)(in[11 + i] >> 8);
    }
}

/* The real name of the entry at `index`, assembled from whatever sits in
   front of it. Returns 0 when there is nothing there, which is every file
   written by something that only ever wrote short names. */
static u32 long_name_of(const dir_t *d, u32 index, const dirent_t *shortent,
                        char *out, u32 cap) {
    if (index == 0) return 0;
    u8 want = short_checksum(shortent->name);

    char buf[FAT_NAME_MAX];
    memset(buf, 0, sizeof buf);
    u32 len = 0;
    bool complete = false;

    for (int i = (int)index - 1; i >= 0; i--) {
        dirent_t raw;
        if (!dir_read(d, (u32)i, &raw)) return 0;
        if ((raw.attr & ATTR_LFN_MASK) != ATTR_LFN) break;

        const lfn_t *l = (const lfn_t *)&raw;
        if (l->checksum != want) return 0;     /* left over from another name */

        u32 seq = l->seq & 0x1F;
        if (seq == 0) return 0;

        u32 at = (seq - 1) * LFN_CHARS;
        if (at >= FAT_NAME_MAX - 1) return 0;

        u16 chars[LFN_CHARS];
        lfn_get(l, chars);
        for (u32 k = 0; k < LFN_CHARS; k++) {
            u32 pos = at + k;
            if (pos >= FAT_NAME_MAX - 1) break;
            u16 ch = chars[k];
            if (ch == 0x0000 || ch == 0xFFFF) continue;
            /* Anything outside ASCII becomes a question mark rather than
               half of a character nobody can type. */
            buf[pos] = (ch < 0x80) ? (char)ch : '?';
            if (pos + 1 > len) len = pos + 1;
        }

        if (l->seq & LFN_LAST) { complete = true; break; }
    }

    if (!complete || !len || len >= cap) return 0;
    memcpy(out, buf, len);
    out[len] = 0;
    return len;
}

static bool same_name(const char *a, const char *b) {
    while (*a && *b) {
        if (upcase(*a) != upcase(*b)) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Whether a name needs the long form at all.
 *
 * Lower case on its own is not a reason. A short name is stored folded and
 * read back folded, which is what every file in this system has always done,
 * and making those long as well would change how existing volumes read. The
 * reasons are the ones that genuinely cannot be written in eight and three:
 * length, spaces, and more than one dot. */
static bool needs_long(const char *name) {
    u32 base = 0, ext = 0;
    bool dot = false, in_ext = false;
    for (u32 i = 0; name[i]; i++) {
        char c = name[i];
        if (c == '.') {
            if (dot) return true;
            dot = true;
            in_ext = true;
            continue;
        }
        if (c == ' ') return true;
        if (in_ext) ext++; else base++;
    }
    return base > 8 || ext > 3;
}

static bool short_taken(const dir_t *d, const u8 name[11]) {
    u32 cap = dir_capacity(d);
    dirent_t e;
    for (u32 i = 0; i < cap; i++) {
        if (!dir_read(d, i, &e)) break;
        if (e.name[0] == ENT_FREE) break;
        if (!entry_is_real(&e)) continue;
        if (memcmp(e.name, name, 11) == 0) return true;
    }
    return false;
}

/* The short name a long one is filed under.
 *
 * The first few usable characters, a tilde and a number, which is what
 * everything else does. The number is what makes it unique, and it has to
 * be, because the short name is the one the file is actually found by. */
static void make_alias(const dir_t *d, const char *name, u8 out[11]) {
    char base[8];
    u32 b = 0;
    for (u32 i = 0; name[i] && b < 6; i++) {
        char c = name[i];
        if (c == '.') break;
        if (c == ' ') continue;
        base[b++] = upcase(c);
    }
    if (!b) base[b++] = 'X';

    const char *dot = 0;
    for (u32 i = 0; name[i]; i++) if (name[i] == '.') dot = name + i;

    for (u32 n = 1; n <= 999; n++) {
        char tail[5];
        u32 t = 0;
        tail[t++] = '~';
        if (n >= 100) tail[t++] = (char)('0' + n / 100);
        if (n >= 10)  tail[t++] = (char)('0' + (n / 10) % 10);
        tail[t++] = (char)('0' + n % 10);

        u32 keep = 8 - t;
        if (keep > b) keep = b;

        memset(out, ' ', 11);
        for (u32 i = 0; i < keep; i++) out[i] = (u8)base[i];
        for (u32 i = 0; i < t; i++) out[keep + i] = (u8)tail[i];
        if (dot)
            for (u32 j = 0; j < 3 && dot[1 + j]; j++)
                out[8 + j] = (u8)upcase(dot[1 + j]);

        if (!short_taken(d, out)) return;
    }
}

/* A run of free slots, growing the directory once if there is not one. */
static int dir_free_run(const dir_t *d, u32 want) {
    for (int attempt = 0; attempt < 2; attempt++) {
        u32 cap = dir_capacity(d);
        u32 run = 0;
        for (u32 i = 0; i < cap; i++) {
            dirent_t e;
            if (!dir_read(d, i, &e)) break;
            if (e.name[0] == ENT_FREE || e.name[0] == ENT_DELETED) {
                if (++run == want) return (int)(i + 1 - want);
            } else {
                run = 0;
            }
        }
        if (attempt > 0 || !dir_grow(d)) return -1;
    }
    return -1;
}

/* Puts a name into a directory and hands back the slot the short entry goes
   in. The caller fills in the rest of that entry and writes it, so a failure
   part way through this leaves entries that describe nothing rather than a
   file with no contents. */
static int dir_put_name(const dir_t *d, const char *name, dirent_t *e) {
    if (!needs_long(name)) {
        int slot = dir_free_slot(d);
        if (slot < 0) return -1;
        to_83(name, e->name);
        return slot;
    }

    u32 chars = 0;
    while (name[chars]) chars++;
    if (chars >= FAT_NAME_MAX) return -1;

    u32 n = (chars + LFN_CHARS - 1) / LFN_CHARS;
    int start = dir_free_run(d, n + 1);
    if (start < 0) return -1;

    make_alias(d, name, e->name);
    u8 sum = short_checksum(e->name);

    /* Numbered backwards, so the entry nearest the short one is the first
       part of the name and the one furthest away is marked as the last. */
    for (u32 i = 0; i < n; i++) {
        lfn_t l;
        memset(&l, 0xFF, sizeof l);
        l.seq = (u8)(n - i);
        if (i == 0) l.seq |= LFN_LAST;
        l.attr = ATTR_LFN;
        l.type = 0;
        l.checksum = sum;
        l.cluster = 0;

        u16 chunk[LFN_CHARS];
        u32 at = (n - i - 1) * LFN_CHARS;
        for (u32 k = 0; k < LFN_CHARS; k++) {
            u32 pos = at + k;
            if (pos < chars)       chunk[k] = (u16)(u8)name[pos];
            else if (pos == chars) chunk[k] = 0x0000;
            else                   chunk[k] = 0xFFFF;
        }
        lfn_put(&l, chunk);

        if (!dir_write(d, (u32)start + i, (const dirent_t *)&l)) return -1;
    }
    return start + (int)n;
}

/* Marks the long entries in front of a short one as gone. Left behind, they
   describe a file that no longer exists. */
static void dir_drop_long(const dir_t *d, u32 index, const dirent_t *shortent) {
    if (index == 0) return;
    u8 sum = short_checksum(shortent->name);
    for (int i = (int)index - 1; i >= 0; i--) {
        dirent_t raw;
        if (!dir_read(d, (u32)i, &raw)) break;
        if ((raw.attr & ATTR_LFN_MASK) != ATTR_LFN) break;
        if (((const lfn_t *)&raw)->checksum != sum) break;
        bool last = (((const lfn_t *)&raw)->seq & LFN_LAST) != 0;
        raw.name[0] = ENT_DELETED;
        dir_write(d, (u32)i, &raw);
        if (last) break;
    }
}

static int dir_find(const dir_t *d, const char *name, dirent_t *out) {
    u8 want[11];
    to_83(name, want);
    u32 cap = dir_capacity(d);
    dirent_t e;
    for (u32 i = 0; i < cap; i++) {
        if (!dir_read(d, i, &e)) break;
        if (e.name[0] == ENT_FREE) break;
        if (!entry_is_real(&e)) continue;
        if (memcmp(e.name, want, 11) == 0) { if (out) *out = e; return (int)i; }

        /* And by the name somebody actually gave it, which is not the one
           stored in this entry when it did not fit. */
        char real[FAT_NAME_MAX];
        if (long_name_of(d, i, &e, real, sizeof real) && same_name(real, name)) {
            if (out) *out = e;
            return (int)i;
        }
    }
    return -1;
}

/* --- paths -------------------------------------------------------------- */

/* Copies the next component of a path into `out` and returns what is left,
   or null once the path is exhausted. Leading and repeated slashes are
   skipped, so "//a///b" reads the same as "/a/b". */
static const char *next_component(const char *p, char *out, u32 cap) {
    while (*p == '/') p++;
    if (!*p) return 0;
    u32 n = 0;
    while (*p && *p != '/') {
        if (n < cap - 1) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return p;
}

/* Walks every component but the last. `leaf` receives the final name, which
   may not exist yet. A trailing slash is ignored. */
static bool resolve_parent(const char *path, dir_t *parent, char *leaf, u32 leaf_cap) {
    dir_t here = ROOT;
    char part[FAT_NAME_MAX], pending[FAT_NAME_MAX];
    bool have_pending = false;

    const char *p = path;
    for (;;) {
        p = next_component(p, part, sizeof(part));
        if (!p) break;

        if (have_pending) {
            /* The previous component was not the last after all, so descend
               into it. */
            if (strcmp(pending, ".") == 0) {
                /* stay */
            } else if (strcmp(pending, "..") == 0) {
                if (here.root) { /* the root is its own parent */ }
                else {
                    dirent_t up;
                    if (dir_find(&here, "..", &up) < 0) return false;
                    u32 upc = ent_cluster(&up);
                    if (upc < 2) here = ROOT;
                    else { here.root = false; here.cluster = upc; }
                }
            } else {
                dirent_t e;
                if (dir_find(&here, pending, &e) < 0) return false;
                if (!(e.attr & ATTR_DIRECTORY)) return false;
                u32 ec = ent_cluster(&e);
                if (ec < 2) here = ROOT;
                else { here.root = false; here.cluster = ec; }
            }
        }
        strncpy(pending, part, sizeof(pending) - 1);
        pending[sizeof(pending) - 1] = 0;
        have_pending = true;
    }

    *parent = here;
    if (have_pending) strncpy(leaf, pending, leaf_cap - 1);
    else              leaf[0] = 0;            /* the path was just "/" */
    leaf[leaf_cap - 1] = 0;
    return true;
}

/* Resolves a whole path to a directory. */
static bool resolve_dir(const char *path, dir_t *out) {
    dir_t parent;
    char leaf[FAT_NAME_MAX];
    if (!resolve_parent(path, &parent, leaf, sizeof(leaf))) return false;
    if (!leaf[0]) { *out = parent; return true; }       /* the root itself */

    if (strcmp(leaf, ".") == 0) { *out = parent; return true; }
    if (strcmp(leaf, "..") == 0) {
        if (parent.root) { *out = ROOT; return true; }
        dirent_t up;
        if (dir_find(&parent, "..", &up) < 0) return false;
        u32 upc = ent_cluster(&up);
        if (upc < 2) *out = ROOT;
        else { out->root = false; out->cluster = upc; }
        return true;
    }

    dirent_t e;
    if (dir_find(&parent, leaf, &e) < 0) return false;
    if (!(e.attr & ATTR_DIRECTORY)) return false;
    u32 ec = ent_cluster(&e);
    if (ec < 2) *out = ROOT;
    else { out->root = false; out->cluster = ec; }
    return true;
}

/* --- listing ------------------------------------------------------------ */

int fat_list(const char *path, u32 index, char *name_out, u32 *size_out, bool *dir_out) {
    if (!mounted) return -1;
    dir_t d;
    if (!resolve_dir(path ? path : "/", &d)) return -1;

    u32 cap = dir_capacity(&d);
    dirent_t e;
    u32 seen = 0;
    for (u32 i = 0; i < cap; i++) {
        if (!dir_read(&d, i, &e)) break;
        if (e.name[0] == ENT_FREE) break;
        if (!entry_is_real(&e)) continue;

        /* "." and ".." are real entries on disk, but listing them is noise. */
        if (e.name[0] == '.') continue;

        if (seen == index) {
            if (name_out) {
                /* The long one if there is one, and the short one otherwise.
                   A caller's buffer is VFS_NAME_MAX, which is what the cap
                   passed here protects. */
                char real[FAT_NAME_MAX];
                if (long_name_of(&d, i, &e, real, FAT_NAME_MAX))
                    strncpy(name_out, real, FAT_NAME_MAX - 1);
                else
                    from_83(e.name, name_out);
            }
            if (size_out) *size_out = e.size;
            if (dir_out)  *dir_out = (e.attr & ATTR_DIRECTORY) != 0;
            return 1;
        }
        seen++;
    }
    return 0;
}

u32 fat_count(const char *path) {
    u32 n = 0;
    while (fat_list(path, n, 0, 0, 0) == 1) n++;
    return n;
}

bool fat_stat(const char *path, u32 *size_out, bool *dir_out) {
    if (!mounted) return false;

    dir_t parent;
    char leaf[FAT_NAME_MAX];
    if (!resolve_parent(path, &parent, leaf, sizeof(leaf))) return false;

    if (!leaf[0] || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
        if (size_out) *size_out = 0;
        if (dir_out)  *dir_out = true;
        return true;
    }

    dirent_t e;
    if (dir_find(&parent, leaf, &e) < 0) return false;
    if (size_out) *size_out = e.size;
    if (dir_out)  *dir_out = (e.attr & ATTR_DIRECTORY) != 0;
    return true;
}

/* --- files -------------------------------------------------------------- */

int fat_read_file(const char *path, u8 *buf, u32 cap) {
    if (!mounted) return -1;

    dir_t parent;
    char leaf[FAT_NAME_MAX];
    if (!resolve_parent(path, &parent, leaf, sizeof(leaf))) return -1;

    dirent_t e;
    if (dir_find(&parent, leaf, &e) < 0) return -1;
    if (e.attr & ATTR_DIRECTORY) return -1;

    u32 want = e.size < cap ? e.size : cap;
    u32 done = 0;
    u32 cluster = ent_cluster(&e);

    while (done < want && cluster >= 2 && cluster < eoc_min()) {
        u32 lba = cluster_lba(cluster);
        for (u32 s = 0; s < sectors_per_cluster && done < want; s++) {
            if (!vol_read(lba + s, 1, sec)) return -1;
            u32 n = want - done;
            if (n > SECTOR_SIZE) n = SECTOR_SIZE;
            memcpy(buf + done, sec, n);
            done += n;
        }
        cluster = fat_get(cluster);
    }
    return (int)done;
}

bool fat_write_file(const char *path, const u8 *buf, u32 size) {
    if (!mounted) return false;

    dir_t parent;
    char leaf[FAT_NAME_MAX];
    if (!resolve_parent(path, &parent, leaf, sizeof(leaf))) return false;
    if (!leaf[0]) return false;

    dirent_t e;
    int slot = dir_find(&parent, leaf, &e);
    u32 old_chain = 0;

    if (slot >= 0) {
        if (e.attr & ATTR_DIRECTORY) return false;
        /* Remember the old chain but do not touch it yet. */
        old_chain = ent_cluster(&e);
    } else {
        memset(&e, 0, sizeof(e));
        slot = dir_put_name(&parent, leaf, &e);
        if (slot < 0) return false;
        e.attr = ATTR_ARCHIVE;
    }

    /* Write the new copy first, into clusters nothing points at yet. Losing
       power during this stage leaves the directory still describing the old
       file, so the old contents survive intact. */
    u32 first = 0, prev = 0, written = 0;
    u32 per_cluster = fat_cluster_bytes();

    while (written < size) {
        u32 c = alloc_cluster();
        if (!c) { if (first) free_chain(first); return false; }
        if (prev) fat_set(prev, c);
        else first = c;
        prev = c;

        u32 lba = cluster_lba(c);
        for (u32 s = 0; s < sectors_per_cluster; s++) {
            memset(sec, 0, SECTOR_SIZE);
            u32 off = written + s * SECTOR_SIZE;
            if (off < size) {
                u32 n = size - off;
                if (n > SECTOR_SIZE) n = SECTOR_SIZE;
                memcpy(sec, buf + off, n);
            }
            if (!vol_write(lba + s, 1, sec)) { if (first) free_chain(first); return false; }
        }
        written += per_cluster;
    }

    /* Make sure the data is on the platter before anything points at it. */
    blk_flush();

    set_ent_cluster(&e, first);
    e.size = size;
    e.write_date = 0x5A21;
    e.write_time = 0;

    /* One sector write swings the file from the old chain to the new one.
       This is the commit: before it the old file is live, after it the new
       one is, and there is no moment where neither is. */
    if (!dir_write(&parent, (u32)slot, &e)) { if (first) free_chain(first); return false; }
    if (!blk_flush()) return false;

    /* Only now is the old chain unreachable and safe to release. A crash
       before this point leaks clusters, which fat_reclaim recovers; it never
       loses the file. */
    if (old_chain >= 2) { free_chain(old_chain); blk_flush(); }
    return true;
}

bool fat_delete_file(const char *path) {
    if (!mounted) return false;

    dir_t parent;
    char leaf[FAT_NAME_MAX];
    if (!resolve_parent(path, &parent, leaf, sizeof(leaf))) return false;

    dirent_t e;
    int slot = dir_find(&parent, leaf, &e);
    if (slot < 0) return false;
    if (e.attr & ATTR_DIRECTORY) return false;      /* rmdir is a different job */

    if (ent_cluster(&e) >= 2) free_chain(ent_cluster(&e));
    dir_drop_long(&parent, (u32)slot, &e);
    e.name[0] = ENT_DELETED;
    if (!dir_write(&parent, (u32)slot, &e)) return false;
    return blk_flush();
}

/* --- making and removing directories ------------------------------------ */

bool fat_mkdir(const char *path) {
    if (!mounted) return false;

    dir_t parent;
    char leaf[FAT_NAME_MAX];
    if (!resolve_parent(path, &parent, leaf, sizeof(leaf))) return false;
    if (!leaf[0] || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) return false;
    if (dir_find(&parent, leaf, 0) >= 0) return false;      /* already there */

    u32 c = alloc_cluster();
    if (!c) return false;
    if (!zero_cluster(c)) { fat_set(c, 0); return false; }

    /* A directory starts with the two entries that make it navigable. ".."
       pointing at cluster 0 means the root, which is what the format says
       even though the root has no cluster of its own. */
    dir_t self = { false, c };
    dirent_t dot;
    memset(&dot, 0, sizeof(dot));
    memset(dot.name, ' ', 11);
    dot.name[0] = '.';
    dot.attr = ATTR_DIRECTORY;
    set_ent_cluster(&dot, c);
    dot.write_date = 0x5A21;
    if (!dir_write(&self, 0, &dot)) { free_chain(c); return false; }

    dot.name[1] = '.';
    /* ".." pointing at the root is written as cluster zero on both, even
       on FAT32 where the root has a real cluster number. The specification
       says so and readers rely on it. */
    set_ent_cluster(&dot, parent.root ? 0 : parent.cluster);
    if (!dir_write(&self, 1, &dot)) { free_chain(c); return false; }

    blk_flush();

    /* Only once the directory is a valid one does anything point at it. */
    dirent_t e;
    memset(&e, 0, sizeof(e));
    int slot = dir_put_name(&parent, leaf, &e);
    if (slot < 0) { free_chain(c); return false; }
    e.attr = ATTR_DIRECTORY;
    set_ent_cluster(&e, c);
    e.size = 0;                       /* directories report zero, by the spec */
    e.write_date = 0x5A21;
    if (!dir_write(&parent, (u32)slot, &e)) { free_chain(c); return false; }
    return blk_flush();
}

bool fat_rmdir(const char *path) {
    if (!mounted) return false;

    dir_t parent;
    char leaf[FAT_NAME_MAX];
    if (!resolve_parent(path, &parent, leaf, sizeof(leaf))) return false;
    if (!leaf[0] || leaf[0] == '.') return false;

    dirent_t e;
    int slot = dir_find(&parent, leaf, &e);
    if (slot < 0) return false;
    if (!(e.attr & ATTR_DIRECTORY)) return false;

    /* Refuse while anything is still inside, rather than orphaning it. */
    if (fat_count(path) > 0) return false;

    u32 ec = ent_cluster(&e);
    if (ec >= 2) free_chain(ec);
    e.name[0] = ENT_DELETED;
    if (!dir_write(&parent, (u32)slot, &e)) return false;
    return blk_flush();
}

/* --- reclaiming leaked clusters ----------------------------------------- */

/* Frees clusters that no directory entry refers to.
 *
 * A crash between writing a new copy and committing it leaves its clusters
 * allocated but unreachable. Nothing is corrupt, but the space is gone until
 * somebody notices, so this runs at mount.
 *
 * Directories are walked with an explicit queue rather than by recursion:
 * the depth is whatever the disk says it is, and a kernel stack is small. */
u32 fat_reclaim(void) {
    if (!mounted) return 0;

    u32 total = cluster_count + 2;
    u8 *reachable = (u8 *)kmalloc(total);
    if (!reachable) return 0;
    memset(reachable, 0, total);
    reachable[0] = reachable[1] = 1;          /* the two reserved entries */

    /* Directories still to visit, by first cluster. On FAT16 the root is not
       among them because it has no chain at all. On FAT32 it does, and it has
       to be marked before anything else: nothing points at the root, so a
       sweep that only follows directory entries never reaches it, decides its
       clusters are stranded, and frees them. The next allocation then hands
       out cluster two and the volume's root is written over by whatever asked
       for space. (Which is what happened the first time this ran on a FAT32
       volume: two files in the root, gone.) */
    u32 queue_cap = 64;
    u32 *queue = (u32 *)kmalloc(queue_cap * 4);
    if (!queue) { kfree(reachable); return 0; }
    u32 head = 0, tail = 0;

    if (fat_bits == 32) {
        u32 c = root_cluster, guard = 0;
        while (c >= 2 && c < eoc_min() && c < total && guard++ < total) {
            if (reachable[c]) break;
            reachable[c] = 1;
            c = fat_get(c);
        }
    }

    dir_t d = ROOT;

    for (;;) {
        u32 cap = dir_capacity(&d);
        dirent_t e;
        for (u32 i = 0; i < cap; i++) {
            if (!dir_read(&d, i, &e)) break;
            if (e.name[0] == ENT_FREE) break;
            if (!entry_is_real(&e)) continue;
            if (e.name[0] == '.') continue;   /* "." and ".." lead in circles */

            u32 c = ent_cluster(&e);
            u32 guard = 0;
            while (c >= 2 && c < eoc_min() && c < total && guard++ < total) {
                if (reachable[c]) break;      /* a loop; stop rather than spin */
                reachable[c] = 1;
                c = fat_get(c);
            }

            if ((e.attr & ATTR_DIRECTORY) && ent_cluster(&e) >= 2) {
                if (tail == queue_cap) {
                    u32 *bigger = (u32 *)kmalloc(queue_cap * 8);
                    if (!bigger) break;       /* stop widening, do not lose data */
                    memcpy(bigger, queue, queue_cap * 4);
                    kfree(queue);
                    queue = bigger;
                    queue_cap *= 2;
                }
                queue[tail++] = ent_cluster(&e);
            }
        }

        if (head == tail) break;
        d.root = false;
        d.cluster = queue[head++];
    }

    u32 freed = 0;
    for (u32 c = 2; c < total; c++) {
        if (reachable[c]) continue;
        if (fat_get(c) == 0) continue;        /* already free */
        fat_set(c, 0);
        freed++;
    }
    if (freed) { blk_flush(); alloc_hint = 2; }

    kfree(queue);
    kfree(reachable);
    return freed;
}

u32 fat_free_bytes(void) {
    if (!mounted) return 0;
    u32 free_clusters = 0;
    for (u32 c = 2; c < cluster_count + 2; c++)
        if (fat_get(c) == 0) free_clusters++;
    return free_clusters * fat_cluster_bytes();
}
