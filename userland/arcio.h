#pragma once
/* Archives on the disk, for the programs that take them apart and make them
 * (Files and the terminal): a file read whole, an entry's name made safe to
 * write, an entry taken out to a file, everything taken out into a folder,
 * the folder that goes to, and a zip made of files and folders. The formats
 * are zip.h's (zip, tar, gzip) and zipw.h's; this is what is done with them
 * on the disk, kept in one place so the two programs do it alike. */
#include "zelr.h"
#include "alloc.h"
#include "zip.h"
#include "zipw.h"

#define ARCIO_PATH      ZELR_PATH_MAX
#define ARCIO_MAX       (64 * 1024 * 1024)          /* an archive read whole */
#define ARCIO_MAKE_FILES 1024
#define ARCIO_MAKE_BYTES (48u * 1024 * 1024)

static inline void arcio_join(char *out, const char *dir, const char *name) {
    int i = 0;
    for (; dir[i] && i < ARCIO_PATH - 2; i++) out[i] = dir[i];
    if (i > 1 && out[i - 1] == '/') i--;          /* no "//" except at the root */
    out[i++] = '/';
    for (int j = 0; name[j] && i < ARCIO_PATH - 1; j++) out[i++] = name[j];
    out[i] = 0;
}

static inline int arcio_ends(const char *s, const char *end) {
    int n = (int)strlen(s), m = (int)strlen(end);
    if (m > n) return 0;
    for (int i = 0; i < m; i++) {
        char a = s[n - m + i], b = end[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* A whole file into memory, a megabyte a call (the kernel's most); 0 when
   it cannot be read or is past ARCIO_MAX. */
static inline u8 *arcio_read_whole(const char *path, u32 *len) {
    zelr_stat st;
    if (stat(path, &st) < 0 || st.is_dir || st.size > ARCIO_MAX) return 0;
    u8 *buf = (u8 *)malloc(st.size + 1);
    if (!buf) return 0;
    int fd = open(path, O_READ);
    if (fd < 0) { free(buf); return 0; }
    u32 at = 0;
    while (at < st.size) {
        int want = st.size - at > ZELR_IO_MAX ? ZELR_IO_MAX : (int)(st.size - at);
        int got = zelr_fread(fd, buf + at, want);
        if (got <= 0) break;
        at += (u32)got;
    }
    close(fd);
    if (at != st.size) { free(buf); return 0; }
    *len = st.size;
    return buf;
}

static inline int arcio_write_whole(const char *path, const u8 *data, int len) {
    int fd = open(path, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) return -1;
    int at = 0;
    while (at < len) {
        int piece = len - at > ZELR_IO_MAX ? ZELR_IO_MAX : len - at;
        int got = zelr_fwrite(fd, data + at, piece);
        if (got <= 0) break;
        at += got;
    }
    close(fd);
    return at == len ? 0 : -1;
}

/* An entry's name as a path below the folder it goes into, or 0 when it
   would reach outside it (from /, through .., or by a drive or a
   backslash). */
static inline int arcio_safe_name(const zip_entry *e, char *out, int cap) {
    if (e->name_len <= 0 || e->name_len >= cap) return 0;
    for (int i = 0; i < e->name_len; i++) {
        char c = e->name[i];
        if (c == '\\' || c == ':' || !c) return 0;
        out[i] = c;
    }
    out[e->name_len] = 0;
    if (out[0] == '/') return 0;
    for (int i = 0; out[i]; ) {
        int j = i;
        while (out[j] && out[j] != '/') j++;
        if (j - i == 2 && out[i] == '.' && out[i + 1] == '.') return 0;
        i = out[j] ? j + 1 : j;
    }
    return 1;
}

/* The folders a path needs, made one by one. */
static inline void arcio_make_parents(const char *path) {
    char part[ARCIO_PATH];
    for (int i = 1; path[i] && i < ARCIO_PATH; i++) {
        if (path[i] != '/') continue;
        memcpy(part, path, i);
        part[i] = 0;
        mkdir(part);
    }
}

/* One entry taken out to a file: its size, or a ZIP_ number. */
static inline int arcio_take_out(const zip_t *z, const zip_entry *e, const char *to) {
    u8 *buf = (u8 *)malloc(e->usize + 1);
    if (!buf) return ZIP_ROOM;
    int got = zip_extract(z, e, buf, e->usize + 1);
    if (got >= 0 && arcio_write_whole(to, buf, got) < 0) got = ZIP_ROOM;
    free(buf);
    if (got < 0) unlink(to);
    return got;
}

/* Where an archive is taken out to when no folder is given: beside it,
   named after it without its ending ("t" for t.zip or t.tar.gz), "t 2"
   and on when that is taken. A .gz alone is one file, and that is its
   name. */
static inline void arcio_dest_for(const char *path, char *out) {
    char dest[ARCIO_PATH];
    strncpy(dest, path, sizeof(dest) - 1);
    dest[sizeof(dest) - 1] = 0;
    int n = (int)strlen(dest);
    if (arcio_ends(dest, ".tar.gz")) dest[n - 7] = 0;
    else if (arcio_ends(dest, ".zip") || arcio_ends(dest, ".tar") || arcio_ends(dest, ".tgz")) dest[n - 4] = 0;
    else if (arcio_ends(dest, ".gz")) dest[n - 3] = 0;
    strcpy(out, dest);
    zelr_stat st;
    for (int k = 2; stat(out, &st) == 0 && k < 100; k++) {
        int m = (int)strlen(dest);
        if (m > ARCIO_PATH - 5) break;
        memcpy(out, dest, m);
        out[m++] = ' ';
        m += utoa((u32)k, out + m);
        out[m] = 0;
    }
}

/* Everything in an archive into a folder that is there already. */
typedef struct {
    int files, refused, why;           /* why: the last ZIP_ number, 0 for a name refused */
} arcio_out;

static inline void arcio_extract_into(const zip_t *z, const char *dest, arcio_out *r) {
    r->files = r->refused = r->why = 0;
    u32 at = 0;
    zip_entry e;
    char name[ARCIO_PATH], to[ARCIO_PATH];
    while (zip_next(z, &at, &e) == 1) {
        if (!arcio_safe_name(&e, name, ARCIO_PATH - (int)strlen(dest) - 2)) { r->refused++; continue; }
        arcio_join(to, dest, name);
        if (e.is_dir) {
            int m = (int)strlen(to);
            if (m > 1 && to[m - 1] == '/') to[m - 1] = 0;
            arcio_make_parents(to);
            mkdir(to);
            continue;
        }
        arcio_make_parents(to);
        int got = arcio_take_out(z, &e, to);
        if (got < 0) { r->refused++; r->why = got; continue; }
        r->files++;
    }
}

/* --- making a zip ---------------------------------------------------------------
 *
 * Files and folders and everything below them: each file deflated when that
 * makes it smaller (zipw.h), folders as entries of their own so an empty one
 * comes back. Read whole and built in memory, so there is a limit, which
 * `over` says was reached. */
typedef struct {
    int files, dirs, over, unread;
    u32 bytes;
} arcio_make_t;

/* Counts what is below path (z 0), or adds it to z under name. */
static inline void arcio_make_walk(zipw_t *z, arcio_make_t *m, const char *path, const char *name, int depth) {
    zelr_stat st;
    if (stat(path, &st) < 0) return;
    if (!st.is_dir) {
        if (!z) {
            m->files++;
            m->bytes += st.size;
            if (m->files > ARCIO_MAKE_FILES || m->bytes > ARCIO_MAKE_BYTES) m->over = 1;
            return;
        }
        u32 dt = 0, dd = zipw_dos_date(1980, 1, 1);
        zelr_times t;
        if (file_times(path, &t) == 0 && t.year) {
            dt = zipw_dos_time(t.hour, t.minute, t.second);
            dd = zipw_dos_date(t.year, t.month, t.day);
        }
        u8 *data = (u8 *)malloc(st.size + 1);
        if (!data) { m->unread++; return; }
        if (slurp(path, (char *)data, (int)st.size + 1) == (int)st.size)
            zipw_add(z, name, data, st.size, dt, dd);
        else
            m->unread++;
        free(data);
        return;
    }
    if (depth > 8) return;
    char sub[ARCIO_PATH], subname[ARCIO_PATH];
    if (!z) m->dirs++;
    else {
        int n = (int)strlen(name);
        if (n + 2 > ARCIO_PATH) return;
        memcpy(subname, name, n);
        subname[n] = '/';
        subname[n + 1] = 0;
        zipw_add(z, subname, 0, 0, 0, zipw_dos_date(1980, 1, 1));
    }
    for (int i = 0; readdir(path, i, &st) == 1; i++) {
        if (!strcmp(st.name, ".") || !strcmp(st.name, "..")) continue;
        arcio_join(sub, path, st.name);
        arcio_join(subname, name, st.name);
        arcio_make_walk(z, m, sub, subname, depth + 1);
    }
}

/* The leaf of a path: its last part, a slash at its end aside. */
static inline const char *arcio_leaf(const char *path) {
    const char *leaf = path;
    for (const char *p = path; *p; p++) if (*p == '/' && p[1]) leaf = p + 1;
    return leaf;
}

/* A zip of paths, each under its own leaf name, in memory: its length, with
   *out the bytes to free, or -1 (m says why: over, unread, or no memory or
   room when neither). */
static inline int arcio_make(const char *const *paths, int n, u8 **out, arcio_make_t *m) {
    m->files = m->dirs = m->over = m->unread = 0;
    m->bytes = 0;
    *out = 0;
    char name[ARCIO_PATH];
    for (int i = 0; i < n; i++) arcio_make_walk(0, m, paths[i], arcio_leaf(paths[i]), 0);
    if (m->over) return -1;
    u32 entries_n = (u32)(m->files + m->dirs);
    u32 cap = m->bytes + entries_n * (30 + ARCIO_PATH) + 4096;
    u32 cd_cap = entries_n * (46 + ARCIO_PATH) + 64;
    u8 *buf = (u8 *)malloc(cap + cd_cap + 22);
    u8 *cd = (u8 *)malloc(cd_cap);
    if (!buf || !cd) { if (buf) free(buf); if (cd) free(cd); return -1; }
    zipw_t z;
    zipw_begin(&z, buf, cap + cd_cap + 22, cd, cd_cap);
    for (int i = 0; i < n; i++) {
        strncpy(name, arcio_leaf(paths[i]), sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        int k = (int)strlen(name);
        if (k > 1 && name[k - 1] == '/') name[k - 1] = 0;
        arcio_make_walk(&z, m, paths[i], name, 0);
    }
    int len = zipw_finish(&z);
    free(cd);
    if (len < 0 || m->unread) { free(buf); return -1; }
    *out = buf;
    return len;
}
