#pragma once
#include "zelr.h"
#include "alloc.h"
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
 * file.
 *
 * Behind the same calls, two more kinds (since 0.209.0). A tar (POSIX ustar,
 * with GNU's long names and pax's path records) is 512-byte headers each
 * followed by its file; it keeps no check of its files' bytes, only of each
 * header. A gzip (RFC 1952) is inflated whole when it is opened, into memory
 * the archive owns (zip_close frees it), and its CRC-32 and length checked
 * there; what comes out is a tar (.tar.gz, .tgz) or a single file (.gz),
 * listed under the name the gzip kept for it. */

#define ZIP_OK       0
#define ZIP_BAD     -1      /* not an archive, or damaged where it says where things are */
#define ZIP_METHOD  -2      /* compressed in a way not read here */
#define ZIP_LOCKED  -3      /* locked with a password */
#define ZIP_BIG     -4      /* ZIP64: past what the 32-bit fields hold */
#define ZIP_CRC     -5      /* taken out, and not what the archive kept */
#define ZIP_ROOM    -6      /* more than there was room for */

#define ZIP_KIND_ZIP 0
#define ZIP_KIND_TAR 1
#define ZIP_KIND_GZ  2      /* one file, gzipped */

typedef struct {
    const u8 *data;
    u32 len;
    u32 cd_off, cd_len;     /* the central directory */
    int count;              /* the files and folders it lists */
    int kind;
    u8 *own;                /* a gzip's inflated bytes, which data then is */
    char gz_name[128];      /* the one file in a .gz */
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
/* --- tar and gzip --------------------------------------------------------- */

/* A tar header is whole when its checksum, the sum of its bytes with the
   checksum's own eight counted as spaces, is what it says. */
static inline int tar_header_ok(const u8 *h) {
    u32 sum = 0, said = 0;
    int digits = 0;
    for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    for (int i = 148; i < 156; i++) {
        if (h[i] >= '0' && h[i] <= '7') { said = said * 8 + (u32)(h[i] - '0'); digits++; }
        else if (digits) break;
    }
    return digits > 0 && sum == said;
}

static inline int tar_looks(const u8 *data, u32 len) {
    return len >= 512 && data[0] && tar_header_ok(data);
}

/* An octal field, or -1 when it is in base 256 (past 8 GiB) or not octal. */
static inline long long tar_octal(const u8 *p, int n) {
    if (p[0] & 0x80) return -1;
    long long v = 0;
    int i = 0;
    while (i < n && p[i] == ' ') i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (p[i] - '0');
    return v;
}

static char zip_tar_name[512];      /* the entry's whole name, given out by zip_next */
static int  zip_long_len;           /* a GNU or pax long name waiting for its entry */
static char zip_long_name[512];

/* A pax record set's path, if it has one, kept for the entry after it. */
static inline void tar_pax_path(const u8 *p, u32 n) {
    u32 at = 0;
    while (at < n) {
        u32 len = 0, k = at;
        while (k < n && p[k] >= '0' && p[k] <= '9') len = len * 10 + (u32)(p[k++] - '0');
        if (!len || len > n - at || k >= n || p[k] != ' ') return;
        const u8 *kv = p + k + 1;
        u32 kvn = at + len - (k + 1);
        if (kvn > 5 && !strncmp((const char *)kv, "path=", 5)) {
            u32 m = kvn - 5;
            if (m && kv[kvn - 1] == '\n') m--;
            if (m > sizeof(zip_long_name) - 2) m = sizeof(zip_long_name) - 2;
            memcpy(zip_long_name, kv + 5, (int)m);
            zip_long_len = (int)m;
        }
        at += len;
    }
}

static inline int tar_next(const zip_t *z, u32 *at, zip_entry *out) {
    if (*at == 0) zip_long_len = 0;
    for (;;) {
        if (*at > z->len || z->len - *at < 512) return 0;
        const u8 *h = z->data + *at;
        int blank = 1;
        for (int i = 0; i < 512 && blank; i++) blank = !h[i];
        if (blank) return 0;                        /* the archive's end */
        if (!tar_header_ok(h)) return ZIP_BAD;
        long long size = tar_octal(h + 124, 12);
        if (size < 0 || size > 0x7FFFFFFFll) return ZIP_BIG;
        u32 data_at = *at + 512;
        if ((u32)size > z->len - data_at) return ZIP_BAD;
        u32 next = data_at + (((u32)size + 511) & ~511u);
        if (next > z->len) next = z->len;
        char type = (char)h[156];
        if (type == 'L') {                          /* GNU: the next entry's long name */
            u32 m = (u32)size;
            if (m > sizeof(zip_long_name) - 2) m = sizeof(zip_long_name) - 2;
            memcpy(zip_long_name, h + 512, (int)m);
            while (m && !zip_long_name[m - 1]) m--;
            zip_long_len = (int)m;
            *at = next;
            continue;
        }
        if (type == 'x') { tar_pax_path(h + 512, (u32)size); *at = next; continue; }
        if (type == 'g' || type == 'K') { *at = next; continue; }

        int n = 0;
        if (zip_long_len) {
            memcpy(zip_tar_name, zip_long_name, zip_long_len);
            n = zip_long_len;
            zip_long_len = 0;
        } else {
            if (!strncmp((const char *)h + 257, "ustar", 5) && h[345]) {
                for (int i = 0; i < 155 && h[345 + i]; i++) zip_tar_name[n++] = (char)h[345 + i];
                zip_tar_name[n++] = '/';
            }
            for (int i = 0; i < 100 && h[i]; i++) zip_tar_name[n++] = (char)h[i];
        }
        *at = next;
        /* Only files and folders: links and devices are passed over. */
        if (type != '0' && type != 0 && type != '5' && type != '7') continue;
        int is_dir = type == '5' || (n && zip_tar_name[n - 1] == '/');
        if (is_dir && n && zip_tar_name[n - 1] != '/') zip_tar_name[n++] = '/';
        zip_tar_name[n] = 0;
        /* "./a.txt", as tar run on "." writes it, is a.txt; "./" is nothing. */
        int skip = 0;
        while (n - skip >= 2 && zip_tar_name[skip] == '.' && zip_tar_name[skip + 1] == '/') skip += 2;
        if (n - skip <= 0) continue;
        out->name = zip_tar_name + skip;
        out->name_len = n - skip;
        out->crc = 0;
        out->csize = out->usize = (u32)size;
        out->local = data_at;
        out->method = 0;
        out->flags = 0;
        out->is_dir = is_dir;
        return 1;
    }
}

/* A gzip member inflated into memory the archive owns, its CRC-32 and length
   checked; the name it kept for its file, or "file". */
static inline int gz_open(zip_t *z, const u8 *data, u32 len) {
    if (len < 18 || data[2] != 8) return ZIP_BAD;
    u8 flags = data[3];
    u32 at = 10;
    if (flags & 4) {                                /* FEXTRA */
        if (at + 2 > len) return ZIP_BAD;
        at += 2 + zip_u16(data + at);
    }
    int named = 0;
    if (flags & 8) {                                /* FNAME */
        int k = 0;
        while (at < len && data[at]) {
            if (k < (int)sizeof(z->gz_name) - 1) z->gz_name[k++] = (char)data[at];
            at++;
        }
        z->gz_name[k] = 0;
        named = k > 0;
        at++;
    }
    if (flags & 16) { while (at < len && data[at]) at++; at++; }   /* FCOMMENT */
    if (flags & 2) at += 2;                         /* FHCRC */
    if (at + 8 > len) return ZIP_BAD;
    if (!named) strcpy(z->gz_name, "file");
    for (char *p = z->gz_name; *p; p++) if (*p == '/' || *p == '\\') *p = '_';
    u32 crc = zip_u32(data + len - 8), size = zip_u32(data + len - 4);
    if (size > 256u * 1024 * 1024) return ZIP_BIG;
    u8 *out = (u8 *)malloc(size + 1);
    if (!out) return ZIP_ROOM;
    int got = inflate_raw(data + at, (int)(len - 8 - at), out, (int)size + 1);
    if (got < 0 || (u32)got != size) { free(out); return got == INF_FULL ? ZIP_BAD : ZIP_CRC; }
    if (zip_crc32(out, size) != crc) { free(out); return ZIP_CRC; }
    z->own = out;
    z->data = out;
    z->len = size;
    return ZIP_OK;
}

static inline void zip_close(zip_t *z) {
    if (z->own) free(z->own);
    z->own = 0;
    z->data = 0;
    z->len = 0;
}

static inline int zip_open(zip_t *z, const u8 *data, u32 len) {
    z->data = data;
    z->len = len;
    z->count = 0;
    z->kind = ZIP_KIND_ZIP;
    z->own = 0;
    z->gz_name[0] = 0;
    if (len >= 2 && data[0] == 0x1f && data[1] == 0x8b) {
        int r = gz_open(z, data, len);
        if (r != ZIP_OK) return r;
        z->kind = tar_looks(z->data, z->len) ? ZIP_KIND_TAR : ZIP_KIND_GZ;
        return ZIP_OK;
    }
    if (len < 22) {
        if (tar_looks(data, len)) { z->kind = ZIP_KIND_TAR; return ZIP_OK; }
        return ZIP_BAD;
    }
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
    if (tar_looks(data, len)) { z->kind = ZIP_KIND_TAR; return ZIP_OK; }
    return ZIP_BAD;
}

/* The next entry in the directory from *at (0 to start), and *at moved past
   it. 1 for an entry, 0 at the end, a ZIP_ number for a damaged directory. */
static inline int zip_next(const zip_t *z, u32 *at, zip_entry *out) {
    if (z->kind == ZIP_KIND_TAR) return tar_next(z, at, out);
    if (z->kind == ZIP_KIND_GZ) {
        if (*at) return 0;
        *at = 1;
        out->name = z->gz_name;
        out->name_len = strlen(z->gz_name);
        out->crc = 0;
        out->csize = out->usize = z->len;
        out->local = 0;
        out->method = 0;
        out->flags = 0;
        out->is_dir = 0;
        return 1;
    }
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
    if (z->kind != ZIP_KIND_ZIP) {
        if (e->usize > cap) return ZIP_ROOM;
        if (e->local > z->len || e->usize > z->len - e->local) return ZIP_BAD;
        memcpy(out, z->data + e->local, (int)e->usize);
        return (int)e->usize;
    }
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
        case ZIP_BIG:    return "it is too big to read here (ZIP64, or a file past 2 GB)";
        case ZIP_CRC:    return "it is damaged: what came out is not what was put in";
        case ZIP_ROOM:   return "there is no room for it";
        default:         return "it is not an archive this can read, or it is damaged";
    }
}
