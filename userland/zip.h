#pragma once
#include "zelr.h"
#include "inflate.h"

/* ZIP archives, read: the list of what is in one, and any of it taken out.
 *
 * Written from PKWARE's APPNOTE. An archive is its files, each behind a
 * local header, and at the end a central directory that lists them all with
 * where each one starts, closed by an end record that says where the
 * directory is. The directory is what is read: it is the archive's own
 * account of itself, and the local headers before each file may leave their
 * sizes out (bit 3 of the flags, for archives written as a stream).
 *
 * Two ways a file is kept are read, which is what nearly every archive
 * holds: stored (0) and deflated (8, inflate.h). Anything else, a file
 * locked with a password, and an archive past four gigabytes (ZIP64) are
 * refused by name rather than read wrongly. A file taken out is checked
 * against the CRC-32 the archive kept for it, so a damaged archive says it
 * is damaged rather than handing over something else.
 *
 * Everything works on the archive's bytes in memory; nothing here reads a
 * file. */

#define ZIP_OK       0
#define ZIP_BAD     -1      /* not an archive, or damaged where it says where things are */
#define ZIP_METHOD  -2      /* compressed in a way not read here */
#define ZIP_LOCKED  -3      /* locked with a password */
#define ZIP_BIG     -4      /* ZIP64: past what the 32-bit fields hold */
#define ZIP_CRC     -5      /* taken out, and not what the archive kept */
#define ZIP_ROOM    -6      /* more than there was room for */

typedef struct {
    const u8 *data;
    u32 len;
    u32 cd_off, cd_len;     /* the central directory */
    int count;              /* the files and folders it lists */
} zip_t;

typedef struct {
    const char *name;       /* not NUL-ended: name_len bytes */
    int  name_len;
    u32  crc, csize, usize;
    u32  local;             /* where its local header is */
    u16  method, flags;
    int  is_dir;            /* a name ending with a slash */
} zip_entry;

static inline u32 zip_u16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static inline u32 zip_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* CRC-32 as ZIP (and PNG, and Ethernet) have it: reflected, 0xEDB88320,
   started and ended with every bit set. A table, made the first time. */
static inline u32 zip_crc32(const u8 *p, u32 n) {
    static u32 table[256];
    static int made;
    if (!made) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = 1;
    }
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* The end record, looked for from the end back past the longest comment an
   archive can carry, and the directory it names checked to lie inside. */
static inline int zip_open(zip_t *z, const u8 *data, u32 len) {
    z->data = data;
    z->len = len;
    z->count = 0;
    if (len < 22) return ZIP_BAD;
    u32 lowest = len > 22 + 65535 ? len - 22 - 65535 : 0;
    for (u32 at = len - 22 + 1; at-- > lowest; ) {
        const u8 *e = data + at;
        if (zip_u32(e) != 0x06054b50u) continue;
        if (at + 22 + zip_u16(e + 20) != len) continue;     /* its comment must end the file */
        u32 count = zip_u16(e + 10), cd_len = zip_u32(e + 12), cd_off = zip_u32(e + 16);
        if (count == 0xFFFF || cd_len == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) return ZIP_BIG;
        if (zip_u16(e + 4) != 0 || zip_u16(e + 6) != 0) return ZIP_BAD;   /* split over disks */
        if (cd_off > at || cd_len > at - cd_off) return ZIP_BAD;
        z->cd_off = cd_off;
        z->cd_len = cd_len;
        z->count = (int)count;
        return ZIP_OK;
    }
    return ZIP_BAD;
}

/* The next entry in the directory from *at (0 to start), and *at moved past
   it. 1 for an entry, 0 at the end, a ZIP_ number for a damaged directory. */
static inline int zip_next(const zip_t *z, u32 *at, zip_entry *out) {
    if (*at >= z->cd_len) return 0;
    if (z->cd_len - *at < 46) return ZIP_BAD;
    const u8 *h = z->data + z->cd_off + *at;
    if (zip_u32(h) != 0x02014b50u) return ZIP_BAD;
    u32 nlen = zip_u16(h + 28), xlen = zip_u16(h + 30), clen = zip_u16(h + 32);
    if (46 + nlen + xlen + clen > z->cd_len - *at) return ZIP_BAD;
    out->flags = (u16)zip_u16(h + 8);
    out->method = (u16)zip_u16(h + 10);
    out->crc = zip_u32(h + 16);
    out->csize = zip_u32(h + 20);
    out->usize = zip_u32(h + 24);
    out->local = zip_u32(h + 42);
    out->name = (const char *)h + 46;
    out->name_len = (int)nlen;
    out->is_dir = nlen > 0 && h[46 + nlen - 1] == '/';
    *at += 46 + nlen + xlen + clen;
    return 1;
}

/* An entry taken out into out (cap bytes): how many bytes, or a ZIP_
   number. Checked against its CRC-32. */
static inline int zip_extract(const zip_t *z, const zip_entry *e, u8 *out, u32 cap) {
    if (e->flags & 1) return ZIP_LOCKED;
    if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu || e->local == 0xFFFFFFFFu) return ZIP_BIG;
    if (e->method != 0 && e->method != 8) return ZIP_METHOD;
    if (e->usize > cap) return ZIP_ROOM;
    if (e->usize > 0x7FFFFFFFu) return ZIP_BIG;
    if (e->local > z->len || z->len - e->local < 30) return ZIP_BAD;
    const u8 *l = z->data + e->local;
    if (zip_u32(l) != 0x04034b50u) return ZIP_BAD;
    u32 start = e->local + 30 + zip_u16(l + 26) + zip_u16(l + 28);
    if (start > z->len || e->csize > z->len - start) return ZIP_BAD;
    const u8 *in = z->data + start;
    int got;
    if (e->method == 0) {
        if (e->csize != e->usize) return ZIP_BAD;
        memcpy(out, in, (int)e->usize);     /* the SDK's: a byte loop becomes a call ring 3 lacks */
        got = (int)e->usize;
    } else {
        got = inflate_raw(in, (int)e->csize, out, (int)cap);
        if (got == INF_FULL) return ZIP_ROOM;
        if (got < 0) return ZIP_BAD;
    }
    if ((u32)got != e->usize) return ZIP_BAD;
    if (zip_crc32(out, (u32)got) != e->crc) return ZIP_CRC;
    return got;
}

/* What a ZIP_ number means, as a person would be told. */
static inline const char *zip_why(int err) {
    switch (err) {
        case ZIP_METHOD: return "it is compressed in a way this cannot read";
        case ZIP_LOCKED: return "it is locked with a password";
        case ZIP_BIG:    return "it is too big (ZIP64)";
        case ZIP_CRC:    return "it is damaged: what came out is not what was put in";
        case ZIP_ROOM:   return "there is no room for it";
        default:         return "it is not an archive this can read, or it is damaged";
    }
}
