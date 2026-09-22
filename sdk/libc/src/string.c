/* The string functions, written out.
 *
 * Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Plainly rather than cleverly. A word-at-a-time memcpy is worth having on
 * a machine that moves megabytes; this one moves kilobytes, and a loop that
 * is obviously right is worth more here than one that is quickly right.
 * memcpy and memset are the exceptions, because the compiler emits calls to
 * them for structure assignment and for clearing an array, so they are on
 * every path whether a program names them or not.
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    /* Eight at a time while both ends are aligned and there is a word left,
       then the tail one byte at a time. */
    if (((unsigned long)d | (unsigned long)s) % sizeof(unsigned long) == 0) {
        unsigned long *dw = (unsigned long *)d;
        const unsigned long *sw = (const unsigned long *)s;
        while (n >= sizeof(unsigned long)) {
            *dw++ = *sw++;
            n -= sizeof(unsigned long);
        }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--) *d++ = *s++;
    return dst;
}

/* Overlapping is the whole reason this exists, so the direction is chosen
   rather than assumed: copying forwards over an overlap that runs the other
   way writes the bytes it is about to read. */
void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) return memcpy(dst, src, n);
    d += n;
    s += n;
    while (n--) *--d = *--s;
    return dst;
}

void *memset(void *p, int c, size_t n) {
    unsigned char *b = (unsigned char *)p;
    unsigned char v = (unsigned char)c;

    if ((unsigned long)b % sizeof(unsigned long) == 0) {
        unsigned long w = 0;
        for (size_t i = 0; i < sizeof(unsigned long); i++) w = (w << 8) | v;
        unsigned long *bw = (unsigned long *)b;
        while (n >= sizeof(unsigned long)) {
            *bw++ = w;
            n -= sizeof(unsigned long);
        }
        b = (unsigned char *)bw;
    }
    while (n--) *b++ = v;
    return p;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

void *memchr(const void *p, int c, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++)
        if (b[i] == (unsigned char)c) return (void *)(b + i);
    return NULL;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

/* The one everybody gets wrong: it copies exactly n bytes, pads with zeros
   when the source is shorter, and does not terminate at all when it is
   longer. That is what the standard says, and a program that relies on the
   padding is relying on something real. */
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *end = dst + strlen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++) end[i] = src[i];
    end[i] = 0;
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

/* Looking for the terminator finds it, which is what strchr(s, 0) means and
   is how a program gets the end of a string in one call. */
char *strchr(const char *s, int c) {
    char want = (char)c;
    for (;; s++) {
        if (*s == want) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    char want = (char)c;
    const char *found = NULL;
    for (;; s++) {
        if (*s == want) found = s;
        if (!*s) return (char *)found;
    }
}

char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (!out) return NULL;
    memcpy(out, s, n);
    return out;
}

static int in_set(char c, const char *set) {
    for (; *set; set++) if (*set == c) return 1;
    return 0;
}

size_t strspn(const char *s, const char *set) {
    size_t n = 0;
    while (s[n] && in_set(s[n], set)) n++;
    return n;
}

size_t strcspn(const char *s, const char *set) {
    size_t n = 0;
    while (s[n] && !in_set(s[n], set)) n++;
    return n;
}

char *strpbrk(const char *s, const char *set) {
    for (; *s; s++) if (in_set(*s, set)) return (char *)s;
    return NULL;
}

/* strtok keeps its place between calls, which is why it is the one function
   in this file that everybody warns you about. It is here because programs
   use it, and leaving it out would not make those programs safer. */
static char *tok_rest;

char *strtok(char *s, const char *sep) {
    if (!s) s = tok_rest;
    if (!s) return NULL;

    s += strspn(s, sep);
    if (!*s) { tok_rest = NULL; return NULL; }

    char *end = s + strcspn(s, sep);
    if (*end) { *end = 0; tok_rest = end + 1; }
    else tok_rest = NULL;
    return s;
}

const char *strerror(int err) {
    switch (err) {
        case 0:       return "no error";
        case EPERM:   return "not permitted";
        case ENOENT:  return "no such file";
        case EIO:     return "input or output error";
        case EBADF:   return "not an open descriptor";
        case ENOMEM:  return "out of memory";
        case EACCES:  return "not allowed";
        case EEXIST:  return "already exists";
        case ENOTDIR: return "not a directory";
        case EISDIR:  return "is a directory";
        case EINVAL:  return "not a valid argument";
        case EMFILE:  return "too many open files";
        case ENOSPC:  return "no room left on the disk";
        case ESPIPE:  return "not something that can seek";
        case ERANGE:  return "out of range";
        case ENOSYS:  return "this machine cannot do that";
        default:      return "unknown error";
    }
}
