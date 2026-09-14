/* See include/parts.h.
 *
 * The care in here is all about refusing to believe a table that does not
 * add up. A partition entry is four numbers, and acting on a wrong one means
 * reading or writing somewhere else on somebody's disk. GPT was designed
 * with that in mind and checksums both its header and its entry array, so
 * there is no excuse for not checking; MBR has nothing of the kind, so the
 * entries are range checked against the disk instead.
 *
 * Sector numbers are 32-bit throughout this kernel, which caps a usable
 * partition at two terabytes from the start of the disk. GPT itself is
 * 64-bit and entries beyond that are read, reported and then skipped rather
 * than silently truncated into a number that points somewhere real. */
#include "parts.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"

/* --- crc32 ---------------------------------------------------------------- */

/* Bitwise rather than table driven: the only things checksummed here are a
   92 byte header and a 16 KiB entry array, once each at boot, and a 1 KiB
   table would cost more than it saves. */
u32 crc32(const void *data, u32 len) {
    const u8 *p = (const u8 *)data;
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (u32)(-(i32)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

/* --- state ---------------------------------------------------------------- */

static part_t        table[PARTS_MAX];
static u32           count;
static part_scheme_t scheme;
static char          gpt_error[64];

part_scheme_t parts_scheme(void) { return scheme; }
u32           parts_count(void)  { return count; }
const char   *parts_gpt_error(void) { return gpt_error; }

const part_t *parts_get(u32 index) {
    return index < count ? &table[index] : 0;
}

const char *parts_scheme_name(void) {
    switch (scheme) {
        case PART_SCHEME_GPT: return "gpt";
        case PART_SCHEME_MBR: return "mbr";
        default:              return "none";
    }
}

/* --- what a partition holds ------------------------------------------------ */

/* Whether the first sector of a partition looks like a FAT volume.
 *
 * The type in an MBR entry and the type GUID in a GPT one both say what the
 * partition was meant to hold, which is not the same as what it does hold.
 * This asks the volume instead: the boot signature, a sector size this
 * kernel can use, and a non-zero cluster size. It is not proof, but it is
 * the same evidence fat_mount goes on, so a partition that passes here and
 * fails there fails for a reason worth printing. */
static bool sector_looks_like_fat(u32 lba) {
    u8 sec[SECTOR_SIZE];
    if (!blk_read(lba, 1, sec)) return false;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return false;

    u16 bytes_per_sector = *(u16 *)(sec + 11);
    u8  spc              = sec[13];
    if (bytes_per_sector != SECTOR_SIZE) return false;
    if (spc == 0 || (spc & (spc - 1)) != 0) return false;   /* must be a power of two */
    if (sec[16] == 0) return false;                          /* no file allocation tables */
    return true;
}

static void add(u32 start, u32 sectors, u8 mbr_type, bool esp, const char *name) {
    if (count >= PARTS_MAX) return;
    part_t *p = &table[count];
    p->start = start;
    p->sectors = sectors;
    p->mbr_type = mbr_type;
    p->efi_system = esp;
    p->looks_like_fat = sector_looks_like_fat(start);
    u32 i = 0;
    for (; name[i] && i < PART_NAME_MAX - 1; i++) p->name[i] = name[i];
    p->name[i] = 0;
    count++;
}

/* --- gpt ------------------------------------------------------------------- */

typedef struct {
    char sig[8];                  /* "EFI PART" */
    u32  revision;
    u32  header_size;
    u32  header_crc;
    u32  reserved;
    u64  my_lba;
    u64  alternate_lba;
    u64  first_usable;
    u64  last_usable;
    u8   disk_guid[16];
    u64  entry_lba;
    u32  entry_count;
    u32  entry_size;
    u32  entry_crc;
} __attribute__((packed)) gpt_header_t;

typedef struct {
    u8  type_guid[16];
    u8  unique_guid[16];
    u64 first_lba;
    u64 last_lba;
    u64 attributes;
    u16 name[36];                 /* UTF-16, little endian */
} __attribute__((packed)) gpt_entry_t;

static const u8 GUID_EFI_SYSTEM[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

static bool guid_is_zero(const u8 *g) {
    for (int i = 0; i < 16; i++) if (g[i]) return false;
    return true;
}

/* GPT names are UTF-16. Anything outside ASCII becomes a question mark
   rather than half a character: this is for a boot log, not for round
   tripping somebody's partition label. */
static void name_from_utf16(const u16 *src, char *out) {
    u32 i = 0;
    for (; i < 36 && src[i]; i++)
        out[i] = (src[i] < 0x20 || src[i] > 0x7E) ? '?' : (char)src[i];
    out[i] = 0;
}

static bool read_gpt(void) {
    u8 sec[SECTOR_SIZE];
    if (!blk_read(1, 1, sec)) { kformat(gpt_error, sizeof(gpt_error), "header unreadable"); return false; }

    gpt_header_t h;
    memcpy(&h, sec, sizeof(h));
    if (memcmp(h.sig, "EFI PART", 8) != 0) return false;      /* simply not GPT */

    if (h.header_size < sizeof(gpt_header_t) || h.header_size > SECTOR_SIZE) {
        kformat(gpt_error, sizeof(gpt_error), "header size %d", h.header_size);
        return false;
    }

    /* The header's own checksum is computed with the checksum field zeroed,
       so it has to be cleared in a copy before checking. */
    u8 probe[SECTOR_SIZE];
    memcpy(probe, sec, SECTOR_SIZE);
    *(u32 *)(probe + 16) = 0;
    if (crc32(probe, h.header_size) != h.header_crc) {
        kformat(gpt_error, sizeof(gpt_error), "header checksum");
        return false;
    }

    if (h.entry_size < sizeof(gpt_entry_t) || h.entry_size > 4096) {
        kformat(gpt_error, sizeof(gpt_error), "entry size %d", h.entry_size);
        return false;
    }
    if (h.entry_count == 0 || h.entry_count > 256) {
        kformat(gpt_error, sizeof(gpt_error), "entry count %d", h.entry_count);
        return false;
    }
    if (h.entry_lba < 2 || h.entry_lba > 0xFFFFFFFFull) {
        kformat(gpt_error, sizeof(gpt_error), "entry array out of reach");
        return false;
    }

    u32 array_bytes = h.entry_count * h.entry_size;
    u32 array_sectors = (array_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;

    /* 256 entries of 128 bytes is 32 KiB, which is the largest this will
       accept and comfortably more than any real disk carries. */
    static u8 array[256 * 128];
    if (array_bytes > sizeof(array)) {
        kformat(gpt_error, sizeof(gpt_error), "entry array too large");
        return false;
    }
    if (!blk_read((u32)h.entry_lba, array_sectors, array)) {
        kformat(gpt_error, sizeof(gpt_error), "entry array unreadable");
        return false;
    }

    /* The array is checksummed too, and this is the check that matters: the
       header being intact says nothing about the entries a partition would
       be read from. */
    if (crc32(array, array_bytes) != h.entry_crc) {
        kformat(gpt_error, sizeof(gpt_error), "entry array checksum");
        return false;
    }

    u32 disk_sectors = blk_sectors();
    for (u32 i = 0; i < h.entry_count && count < PARTS_MAX; i++) {
        gpt_entry_t e;
        memcpy(&e, array + (u64)i * h.entry_size, sizeof(e));

        if (guid_is_zero(e.type_guid)) continue;             /* an unused slot */
        if (e.last_lba < e.first_lba) continue;

        /* Past what a 32-bit sector number can name. Skipped rather than
           truncated: a truncated start is a valid looking address that
           points at somebody else's data. */
        if (e.first_lba > 0xFFFFFFFFull || e.last_lba > 0xFFFFFFFFull) continue;

        u32 start = (u32)e.first_lba;
        u32 sectors = (u32)(e.last_lba - e.first_lba + 1);
        if (disk_sectors && (start >= disk_sectors || sectors > disk_sectors - start))
            continue;

        char name[PART_NAME_MAX];
        name_from_utf16(e.name, name);
        if (!name[0]) kformat(name, sizeof(name), "partition %d", i + 1);

        add(start, sectors, 0, memcmp(e.type_guid, GUID_EFI_SYSTEM, 16) == 0, name);
    }

    scheme = PART_SCHEME_GPT;
    return true;
}

/* --- mbr ------------------------------------------------------------------- */

static const char *mbr_type_name(u8 t) {
    switch (t) {
        case 0x01: return "fat12";
        case 0x04: case 0x06: case 0x0E: return "fat16";
        case 0x0B: case 0x0C: return "fat32";
        case 0x07: return "ntfs or exfat";
        case 0x83: return "linux";
        case 0x82: return "linux swap";
        case 0xEF: return "efi system";
        case 0x05: case 0x0F: return "extended";
        default:   return "unknown";
    }
}

static bool read_mbr(void) {
    u8 sec[SECTOR_SIZE];
    if (!blk_read(0, 1, sec)) return false;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return false;

    u32 disk_sectors = blk_sectors();
    bool any = false;

    for (u32 i = 0; i < 4; i++) {
        const u8 *e = sec + 446 + i * 16;
        u8 type = e[4];
        if (type == 0) continue;

        /* A protective MBR. It means the real table is a GPT that has
           already been read or already been rejected, and the entry covers
           the whole disk to stop an MBR-only system writing to it. Taking it
           at face value would hand out the disk as one big partition. */
        if (type == 0xEE) continue;

        u32 start = *(const u32 *)(e + 8);
        u32 sectors = *(const u32 *)(e + 12);
        if (!start || !sectors) continue;
        if (disk_sectors && (start >= disk_sectors || sectors > disk_sectors - start))
            continue;

        /* Extended partitions are a linked list inside the disk and nothing
           here follows it. Reporting the container as a volume would be
           wrong, so it is named and skipped. */
        if (type == 0x05 || type == 0x0F) continue;

        add(start, sectors, type, type == 0xEF, mbr_type_name(type));
        any = true;
    }

    if (any) scheme = PART_SCHEME_MBR;
    return any;
}

/* --- the one entry point --------------------------------------------------- */

void parts_scan(void) {
    count = 0;
    scheme = PART_SCHEME_NONE;
    gpt_error[0] = 0;

    if (!blk_present()) return;

    /* GPT first. A GPT disk always carries an MBR as well, so asking the
       other way round finds the decoy. */
    if (read_gpt()) return;
    if (read_mbr()) return;

    /* Neither. An unpartitioned volume, which is what a disk image is and
       what this kernel produces when it formats a blank disk. */
    scheme = PART_SCHEME_NONE;
}
