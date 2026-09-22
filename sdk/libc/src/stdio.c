/* Files, and the formatted printing everything else is written with.
 *
 * Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A FILE is a descriptor, one buffer and the flags that say how the last
 * read ended. Reading and writing share the buffer, and a FILE is in one
 * mode or the other at a time: switching flushes. That is simpler than the
 * standard's full arrangement and it is the part of it programs use.
 *
 * Everything formatted goes through one routine, which writes into a sink
 * rather than into a buffer: printf sends to a FILE and snprintf counts
 * into a caller's array, and both are the same code. The alternative is
 * two implementations of the same nine conversions, which drift.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* The build script sets both of these; they are here so that this
   file is also correct when compiled on its own. */
#ifndef ZELR_NO_SUGAR
#define ZELR_NO_SUGAR 1
#endif
#ifndef ZELR_NO_START
#define ZELR_NO_START 1
#endif
#include "zelr.h"

#define FBUF 4096

struct _zfile {
    int  fd;
    int  used;                   /* this slot is a live FILE */
    int  reading;                /* the buffer holds what was read */
    int  writing;                /* the buffer holds what is to be written */
    int  eof;
    int  err;
    int  no_close;               /* stdin, stdout and stderr outlive main */
    int  unget;                  /* a pushed-back character, or -1 */
    long pos;                    /* where the descriptor is, for ftell */
    int  len;                    /* bytes in the buffer */
    int  at;                     /* how far through them */
    unsigned char buf[FBUF];
};

/* Three that are always open, and room for what a program opens itself. */
static FILE files[FOPEN_MAX];

FILE *stdin  = &files[0];
FILE *stdout = &files[1];
FILE *stderr = &files[2];

/* The console rather than a file: the kernel writes a descriptor with
   fwrite and writes the screen with write(), and 0/1/2 are not descriptors
   this machine hands out. -1 here means "the console". */
#define FD_CONSOLE (-1)

void _zstdio_start(void) {
    for (int i = 0; i < FOPEN_MAX; i++) files[i].unget = -1;

    files[0].used = 1; files[0].fd = FD_CONSOLE; files[0].no_close = 1;
    files[1].used = 1; files[1].fd = FD_CONSOLE; files[1].no_close = 1;
    files[2].used = 1; files[2].fd = FD_CONSOLE; files[2].no_close = 1;
}

/* --- the descriptor underneath ------------------------------------------- */

static int raw_write(FILE *f, const unsigned char *p, int n) {
    if (f->fd == FD_CONSOLE) return write((const char *)p, n);
    return zelr_fwrite(f->fd, p, n);
}

static int raw_read(FILE *f, unsigned char *p, int n) {
    if (f->fd == FD_CONSOLE) return 0;      /* nothing reads the console yet */
    return zelr_fread(f->fd, p, n);
}

int fflush(FILE *f) {
    if (!f) {
        int bad = 0;
        for (int i = 0; i < FOPEN_MAX; i++)
            if (files[i].used && fflush(&files[i]) != 0) bad = 1;
        return bad ? EOF : 0;
    }
    if (!f->used || !f->writing || f->len == 0) return 0;

    int sent = raw_write(f, f->buf, f->len);
    int wanted = f->len;
    f->len = 0;
    f->at = 0;
    if (sent != wanted) { f->err = 1; return EOF; }
    f->pos += sent;
    return 0;
}

void _zflush_all(void) { fflush(NULL); }

/* Leaving read mode throws away what was read ahead, which means the
   descriptor is further on than the program thinks. Putting it back is what
   makes ftell honest after a read followed by a write. */
static void leave_read(FILE *f) {
    if (!f->reading) return;
    int ahead = f->len - f->at;
    if (ahead > 0 && f->fd != FD_CONSOLE) seek(f->fd, -ahead, SEEK_CUR);
    f->reading = 0;
    f->len = f->at = 0;
}

/* --- opening and closing -------------------------------------------------- */

static FILE *free_slot(void) {
    for (int i = 3; i < FOPEN_MAX; i++)
        if (!files[i].used) return &files[i];
    errno = EMFILE;
    return NULL;
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) { errno = EINVAL; return NULL; }

    u32 flags = 0;
    int append = 0;
    switch (mode[0]) {
        case 'r': flags = O_READ; break;
        case 'w': flags = O_WRITE | O_CREATE | O_TRUNC; break;
        case 'a': flags = O_WRITE | O_CREATE | O_APPEND; append = 1; break;
        default: errno = EINVAL; return NULL;
    }
    /* "r+" and friends want both ways. "b" means nothing here: there is no
       text mode to be different from. */
    for (const char *m = mode + 1; *m; m++)
        if (*m == '+') flags |= O_READ | O_WRITE;

    FILE *f = free_slot();
    if (!f) return NULL;

    int fd = open(path, flags);
    if (fd < 0) { errno = ENOENT; return NULL; }

    memset(f, 0, sizeof(*f));
    f->fd = fd;
    f->used = 1;
    f->unget = -1;
    if (append) f->pos = seek(fd, 0, SEEK_END);
    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *f) {
    if (f) fclose(f);
    return fopen(path, mode);
}

int fclose(FILE *f) {
    if (!f || !f->used) return EOF;
    int rc = fflush(f);
    if (!f->no_close && f->fd >= 0) close(f->fd);
    if (f->no_close) {
        f->len = f->at = 0;
        f->reading = f->writing = 0;
    } else {
        f->used = 0;
    }
    return rc;
}

/* --- reading -------------------------------------------------------------- */

static int fill(FILE *f) {
    if (f->writing) { fflush(f); f->writing = 0; }
    f->reading = 1;
    f->at = 0;
    f->len = raw_read(f, f->buf, FBUF);
    if (f->len <= 0) {
        f->len = 0;
        f->eof = 1;
        return 0;
    }
    return f->len;
}

int fgetc(FILE *f) {
    if (!f || !f->used) return EOF;
    if (f->unget >= 0) {
        int c = f->unget;
        f->unget = -1;
        f->pos++;
        return c;
    }
    if (f->at >= f->len && !fill(f)) return EOF;
    f->pos++;
    return f->buf[f->at++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    if (!f || !f->used || c == EOF || f->unget >= 0) return EOF;
    f->unget = (unsigned char)c;
    f->eof = 0;
    if (f->pos > 0) f->pos--;
    return (unsigned char)c;
}

char *fgets(char *buf, int cap, FILE *f) {
    if (!buf || cap <= 0 || !f || !f->used) return NULL;
    int n = 0;
    while (n < cap - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return NULL;
    buf[n] = 0;
    return buf;
}

size_t fread(void *p, size_t size, size_t count, FILE *f) {
    if (!p || !f || !f->used || size == 0) return 0;
    unsigned char *out = (unsigned char *)p;
    size_t want = size * count;
    size_t got = 0;

    while (got < want) {
        if (f->unget >= 0) {
            out[got++] = (unsigned char)f->unget;
            f->unget = -1;
            f->pos++;
            continue;
        }
        if (f->at >= f->len && !fill(f)) break;

        size_t have = (size_t)(f->len - f->at);
        size_t take = want - got < have ? want - got : have;
        memcpy(out + got, f->buf + f->at, take);
        f->at += (int)take;
        f->pos += (long)take;
        got += take;
    }
    return got / size;
}

/* --- writing -------------------------------------------------------------- */

int fputc(int c, FILE *f) {
    if (!f || !f->used) return EOF;
    leave_read(f);
    f->writing = 1;

    f->buf[f->len++] = (unsigned char)c;

    /* A line at a time to the console, so a program that prints and then
       works for a second is read as it goes rather than all at the end. */
    if (f->len >= FBUF || (f->fd == FD_CONSOLE && c == '\n'))
        if (fflush(f) != 0) return EOF;
    return (unsigned char)c;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }

size_t fwrite(const void *p, size_t size, size_t count, FILE *f) {
    if (!p || !f || !f->used || size == 0) return 0;
    const unsigned char *in = (const unsigned char *)p;
    size_t want = size * count;

    leave_read(f);
    f->writing = 1;

    size_t done = 0;
    while (done < want) {
        size_t room = (size_t)(FBUF - f->len);
        if (room == 0) {
            if (fflush(f) != 0) return done / size;
            room = FBUF;
        }
        size_t take = want - done < room ? want - done : room;
        memcpy(f->buf + f->len, in + done, take);
        f->len += (int)take;
        done += take;
    }

    if (f->fd == FD_CONSOLE && memchr(in, '\n', want))
        fflush(f);
    return done / size;
}

int fputs(const char *s, FILE *f) {
    if (!s) return EOF;
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

/* The standard's puts adds a newline; fputs does not. Getting that the wrong
   way round is the commonest single bug in a hand-written libc. */
int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

/* --- where in the file ---------------------------------------------------- */

int fseek(FILE *f, long off, int whence) {
    if (!f || !f->used) return -1;
    if (f->fd == FD_CONSOLE) { errno = ESPIPE; return -1; }

    fflush(f);
    f->reading = f->writing = 0;
    f->len = f->at = 0;
    f->unget = -1;
    f->eof = 0;

    int where = seek(f->fd, (int)off, whence);
    if (where < 0) return -1;
    f->pos = where;
    return 0;
}

long ftell(FILE *f) {
    if (!f || !f->used) return -1;
    if (f->fd == FD_CONSOLE) { errno = ESPIPE; return -1; }
    return f->pos;
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); }

int  feof(FILE *f)     { return f && f->used ? f->eof : 0; }
int  ferror(FILE *f)   { return f && f->used ? f->err : 0; }
void clearerr(FILE *f) { if (f && f->used) { f->eof = 0; f->err = 0; } }

int remove(const char *path) { return unlink(path) == 0 ? 0 : -1; }

int rename(const char *from, const char *to) {
    return zelr_rename(from, to) == 0 ? 0 : -1;
}

void perror(const char *what) {
    if (what && *what) { fputs(what, stderr); fputs(": ", stderr); }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

/* --- one formatter ---------------------------------------------------------
 *
 * Everything that formats writes through this. `out` is where a character
 * goes and `count` is how many have been asked for, which is what snprintf
 * has to return even when its buffer was too small.
 */

typedef struct {
    FILE          *f;            /* a file, or */
    char          *mem;          /* a caller's array */
    size_t         cap;          /* how much of it there is */
    size_t         count;        /* characters asked for, capped or not */
} sink;

static void emit(sink *k, char c) {
    if (k->f) fputc(c, k->f);
    else if (k->mem && k->count + 1 < k->cap) k->mem[k->count] = c;
    k->count++;
}

static void emit_str(sink *k, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) emit(k, s[i]);
}

static void pad(sink *k, char c, int n) {
    for (int i = 0; i < n; i++) emit(k, c);
}

static int digits_of(unsigned long long v, int base, int upper, char *out) {
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = set[v % (unsigned)base]; v /= (unsigned)base; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

/* Six places after the point and no rounding beyond them, which is what %f
   means and as far as this goes: an exact decimal for a binary fraction is
   a much larger piece of work than anything else in this file. */
static void put_double(sink *k, double v, int width, int prec, int left,
                       int zero, int plus) {
    char body[512];
    int n = 0;
    int neg = 0;

    if (__builtin_isnan(v)) { emit_str(k, "nan", 3); return; }
    if (v < 0) { neg = 1; v = -v; }
    if (__builtin_isinf(v)) {
        if (neg) body[n++] = '-';
        else if (plus) body[n++] = '+';
        emit_str(k, body, (size_t)n);
        emit_str(k, "inf", 3);
        return;
    }

    if (prec < 0) prec = 6;
    if (prec > 17) prec = 17;

    /* Round at the place being printed, so 0.5 with no places is 1 rather
       than 0 and 1.9999995 with six is 2.000000 rather than 1.999999. */
    double half = 0.5;
    for (int i = 0; i < prec; i++) half /= 10;
    v += half;

    unsigned long long whole = (unsigned long long)v;
    double frac = v - (double)whole;

    if (neg) body[n++] = '-';
    else if (plus) body[n++] = '+';
    n += digits_of(whole, 10, 0, body + n);

    if (prec > 0) {
        body[n++] = '.';
        for (int i = 0; i < prec; i++) {
            frac *= 10;
            int d = (int)frac;
            if (d < 0) d = 0;
            if (d > 9) d = 9;
            body[n++] = (char)('0' + d);
            frac -= d;
        }
    }

    int fill = width - n;
    if (!left && fill > 0) pad(k, zero ? '0' : ' ', fill);
    emit_str(k, body, (size_t)n);
    if (left && fill > 0) pad(k, ' ', fill);
}

static int format(sink *k, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(k, *fmt); continue; }
        fmt++;
        if (*fmt == '%') { emit(k, '%'); continue; }

        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }

        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { left = 1; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        /* h and hh are read and ignored: the value arrives promoted to an
           int either way, and the cast below is what narrows it. */
        int longs = 0, size_t_arg = 0;
        for (;;) {
            if (*fmt == 'l') { longs++; fmt++; }
            else if (*fmt == 'h') { fmt++; }
            else if (*fmt == 'z') { size_t_arg = 1; fmt++; }
            else if (*fmt == 'j' || *fmt == 't') { longs = 2; fmt++; }
            else break;
        }

        char conv = *fmt;
        char body[72];
        int n = 0;
        int is_num = 0;

        switch (conv) {
            case 'c':
                body[n++] = (char)va_arg(ap, int);
                break;

            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                size_t len = prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s);
                int fill = width - (int)len;
                if (!left && fill > 0) pad(k, ' ', fill);
                emit_str(k, s, len);
                if (left && fill > 0) pad(k, ' ', fill);
                continue;
            }

            case 'd': case 'i': {
                long long v = longs >= 2 || size_t_arg ? va_arg(ap, long long)
                            : longs == 1 ? va_arg(ap, long)
                            : va_arg(ap, int);
                unsigned long long u = v < 0 ? (unsigned long long)(-v)
                                             : (unsigned long long)v;
                if (v < 0) body[n++] = '-';
                else if (plus) body[n++] = '+';
                else if (space) body[n++] = ' ';
                n += digits_of(u, 10, 0, body + n);
                is_num = 1;
                break;
            }

            case 'u': case 'x': case 'X': case 'o': {
                unsigned long long u =
                    longs >= 2 || size_t_arg ? va_arg(ap, unsigned long long)
                    : longs == 1 ? va_arg(ap, unsigned long)
                    : va_arg(ap, unsigned int);
                int base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
                if (alt && base == 16 && u) {
                    body[n++] = '0';
                    body[n++] = conv == 'X' ? 'X' : 'x';
                }
                n += digits_of(u, base, conv == 'X', body + n);
                is_num = 1;
                break;
            }

            case 'p': {
                void *v = va_arg(ap, void *);
                body[n++] = '0';
                body[n++] = 'x';
                n += digits_of((unsigned long long)v, 16, 0, body + n);
                break;
            }

            case 'f': case 'F': case 'g': case 'G': case 'e': case 'E':
                put_double(k, va_arg(ap, double), width, prec, left, zero, plus);
                continue;

            case 'n': {
                int *where = va_arg(ap, int *);
                if (where) *where = (int)k->count;
                continue;
            }

            case 0:
                return (int)k->count;    /* a % at the very end */

            default:
                emit(k, '%');
                emit(k, conv);
                continue;
        }

        /* A precision on a number is a minimum number of digits, which is
           zeros in front of it and not spaces. */
        int lead = 0;
        if (is_num && prec > 0) {
            int sign = (body[0] == '-' || body[0] == '+' || body[0] == ' ') ? 1 : 0;
            int have = n - sign;
            if (prec > have) lead = prec - have;
        }

        int fill = width - n - lead;
        if (!left && fill > 0 && zero && prec < 0) {
            /* Zero padding goes after the sign, not in front of it. */
            int sign = (body[0] == '-' || body[0] == '+' || body[0] == ' ') ? 1 : 0;
            emit_str(k, body, (size_t)sign);
            pad(k, '0', fill);
            emit_str(k, body + sign, (size_t)(n - sign));
        } else {
            if (!left && fill > 0) pad(k, ' ', fill);
            int sign = (body[0] == '-' || body[0] == '+' || body[0] == ' ') ? 1 : 0;
            emit_str(k, body, (size_t)sign);
            pad(k, '0', lead);
            emit_str(k, body + sign, (size_t)(n - sign));
            if (left && fill > 0) pad(k, ' ', fill);
        }
    }
    return (int)k->count;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    sink k;
    k.f = f; k.mem = NULL; k.cap = 0; k.count = 0;
    return format(&k, fmt, ap);
}

int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    sink k;
    k.f = NULL; k.mem = out; k.cap = cap; k.count = 0;
    int n = format(&k, fmt, ap);
    if (out && cap) out[k.count < cap ? k.count : cap - 1] = 0;
    return n;
}

int vsprintf(char *out, const char *fmt, va_list ap) {
    return vsnprintf(out, (size_t)-1, fmt, ap);
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, (size_t)-1, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}
