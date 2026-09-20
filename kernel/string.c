#include "string.h"

/* The volatile pointers here are load-bearing. Without them the compiler
   recognises each of these loops as the very function it is compiling and
   replaces the body with a call to itself, which recurses until the kernel
   stack is gone. */
/* Eight bytes at a time.
 *
 * These were byte loops with volatile on both pointers, which is the one
 * thing guaranteed to stop a compiler widening them. It cost most where it
 * showed least: the compositor copies the whole screen through memcpy, and
 * on a 2048x1536 panel that is twelve million byte-sized writes to a device
 * on the far side of PCIe, once per frame. The volatile bought nothing.
 * Ordering against a device needs a barrier, which this never was. */
void *memset(void *d, int c, size_t n) {
    u8 *p = d;
    u8 b = (u8)c;

    /* Line up on an eight byte boundary first. A store that straddles two
       of them costs more than the bytes it saves, and across a framebuffer
       it breaks the burst that makes write combining worth having. */
    while (n && ((u64)p & 7)) { *p++ = b; n--; }

    u64 wide = 0x0101010101010101ull * b;
    u64 *q = (u64 *)p;
    while (n >= 8) { *q++ = wide; n -= 8; }

    p = (u8 *)q;
    while (n--) *p++ = b;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    u8 *dp = d;
    const u8 *sp = s;

    while (n && ((u64)dp & 7)) { *dp++ = *sp++; n--; }

    /* The source may still be crooked after that. An unaligned load costs
       something on this architecture but it is allowed, and it is far less
       than the seven extra stores the alternative would need. */
    u64 *dw = (u64 *)dp;
    const u64 *sw = (const u64 *)sp;
    while (n >= 8) { *dw++ = *sw++; n -= 8; }

    dp = (u8 *)dw;
    sp = (const u8 *)sw;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    u8 *dp = d;
    const u8 *sp = s;
    if (!n || dp == sp) return d;

    /* Forward is safe whenever the destination starts below the source, or
       far enough above it that the two never touch. */
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);

    dp += n; sp += n;
    while (n--) *--dp = *--sp;
    return d;
}
/* Eight bytes at a time where both sides are lined up.
 *
 * This is asked about whole framebuffers now -- every frame is compared
 * against the one the card was last given so that only the difference is
 * sent -- and a byte at a time is three million iterations for a screen
 * that has not changed. Words make it eight times fewer, and the answer for
 * the one word that differs is still found a byte at a time, because the
 * sign of the result is part of what memcmp means. */
int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;

    if (n >= 8 && (((u64)x ^ (u64)y) & 7) == 0) {
        while (n && ((u64)x & 7)) {
            if (*x != *y) return *x - *y;
            x++; y++; n--;
        }
        const u64 *xw = (const u64 *)x, *yw = (const u64 *)y;
        while (n >= 8 && *xw == *yw) { xw++; yw++; n -= 8; }
        x = (const u8 *)xw; y = (const u8 *)yw;
    }

    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (u8)*a - (u8)*b : 0;
}
char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) { }
    return r;
}
char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n && *s) { *d++ = *s++; n--; }
    while (n--) *d++ = 0;
    return r;
}
char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c ? 0 : (char *)s;
}
