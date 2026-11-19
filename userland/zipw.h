/* A .zip written, which zip.h reads and so does every other system.
 *
 * Built in memory: each file's local header and data in one buffer, its
 * central directory entry in another, the two joined with the end record
 * by zipw_finish. A file is deflated (deflate.h) when that makes it
 * smaller, and kept as it is when it does not. No ZIP64, so 4 GB and 65535
 * entries are the limits, far past what is read whole here; no passwords.
 */
#pragma once
#include "zip.h"
#include "deflate.h"

typedef struct {
    u8 *buf; u32 cap, n;                /* the local headers and the data */
    u8 *cd;  u32 cd_cap, cd_n;          /* the central directory */
    int count;
    int stored;                         /* how many were kept as they were */
    int err;                            /* ZIP_ROOM once something did not fit */
} zipw_t;

static inline void zipw_16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void zipw_32(u8 *p, u32 v) { zipw_16(p, v & 0xFFFF); zipw_16(p + 2, v >> 16); }

/* The DOS date and time a zip keeps: two seconds at a time, from 1980. */
static inline u32 zipw_dos_time(int hour, int min, int sec) { return (u32)(hour << 11 | min << 5 | sec / 2); }
static inline u32 zipw_dos_date(int year, int month, int day) {
    if (year < 1980) return 1 << 5 | 1;
    return (u32)((year - 1980) << 9 | month << 5 | day);
}

static inline void zipw_begin(zipw_t *z, u8 *buf, u32 cap, u8 *cd, u32 cd_cap) {
    z->buf = buf; z->cap = cap; z->n = 0;
    z->cd = cd; z->cd_cap = cd_cap; z->cd_n = 0;
    z->count = z->stored = z->err = 0;
}

/* One entry: a file's bytes under name (a / between folders), or a folder
   when name ends in / and len is 0. */
static inline int zipw_add(zipw_t *z, const char *name, const u8 *data, u32 len, u32 dtime, u32 ddate) {
    u32 nl = 0;
    while (name[nl]) nl++;
    if (z->err) return z->err;
    if (z->n + 30 + nl > z->cap || z->cd_n + 46 + nl > z->cd_cap || nl > 0xFFFF) return z->err = ZIP_ROOM;
    u32 at = z->n, body = at + 30 + nl;
    u32 crc = zip_crc32(data, len);
    int method = 8;
    int c = len ? deflate_raw(data, len, z->buf + body, z->cap - body) : -1;
    if (c < 0) {
        method = 0;
        if (body + len > z->cap) return z->err = ZIP_ROOM;
        for (u32 i = 0; i < len; i++) z->buf[body + i] = data[i];
        c = (int)len;
        if (len) z->stored++;
    }
    u8 *h = z->buf + at;
    zipw_32(h, 0x04034B50);
    zipw_16(h + 4, 20);                 /* the version needed: deflate */
    zipw_16(h + 6, 0);
    zipw_16(h + 8, (u32)method);
    zipw_16(h + 10, dtime);
    zipw_16(h + 12, ddate);
    zipw_32(h + 14, crc);
    zipw_32(h + 18, (u32)c);
    zipw_32(h + 22, len);
    zipw_16(h + 26, nl);
    zipw_16(h + 28, 0);
    for (u32 i = 0; i < nl; i++) h[30 + i] = (u8)name[i];
    z->n = body + (u32)c;

    u8 *d = z->cd + z->cd_n;
    zipw_32(d, 0x02014B50);
    zipw_16(d + 4, 20);                 /* made by: MS-DOS and the like, 2.0 */
    zipw_16(d + 6, 20);
    zipw_16(d + 8, 0);
    zipw_16(d + 10, (u32)method);
    zipw_16(d + 12, dtime);
    zipw_16(d + 14, ddate);
    zipw_32(d + 16, crc);
    zipw_32(d + 20, (u32)c);
    zipw_32(d + 24, len);
    zipw_16(d + 28, nl);
    zipw_16(d + 30, 0);
    zipw_16(d + 32, 0);
    zipw_16(d + 34, 0);
    zipw_16(d + 36, 0);
    zipw_32(d + 38, nl && name[nl - 1] == '/' ? 0x10 : 0);   /* the DOS folder bit */
    zipw_32(d + 42, at);
    for (u32 i = 0; i < nl; i++) d[46 + i] = (u8)name[i];
    z->cd_n += 46 + nl;
    z->count++;
    return ZIP_OK;
}

/* The central directory and the end record after the data; the archive's
   length, or ZIP_ROOM. */
static inline int zipw_finish(zipw_t *z) {
    if (z->err) return z->err;
    if (z->n + z->cd_n + 22 > z->cap || z->count > 0xFFFF) return z->err = ZIP_ROOM;
    u32 cd_at = z->n;
    for (u32 i = 0; i < z->cd_n; i++) z->buf[z->n + i] = z->cd[i];
    z->n += z->cd_n;
    u8 *e = z->buf + z->n;
    zipw_32(e, 0x06054B50);
    zipw_16(e + 4, 0);
    zipw_16(e + 6, 0);
    zipw_16(e + 8, (u32)z->count);
    zipw_16(e + 10, (u32)z->count);
    zipw_32(e + 12, z->cd_n);
    zipw_32(e + 16, cd_at);
    zipw_16(e + 20, 0);
    z->n += 22;
    return (int)z->n;
}
