/* Formatted output.
 *
 * One formatter drives two sinks: the console (screen plus serial, so a
 * headless run sees exactly what a person at the machine would) and a caller
 * supplied buffer, which the graphical programs need because they draw
 * strings rather than print them. */
#include "printf.h"
#include "vga.h"
#include "fb.h"
#include "fbcon.h"
#include "serial.h"
#include "string.h"
#include "blackbox.h"
#include "io.h"

void kputc(char c) {
    /* Graphics mode takes over once it is up; before that, and if the
       card refuses a mode, output stays on the VGA text console. */
    if (fb_active()) fbcon_putc(c);
    else             vga_putc(c);

    if (c == '\n') serial_putc('\r');
    serial_putc(c);
}

void kputs(const char *s) { while (*s) kputc(*s++); }

/* Where a formatted run is going. A null buffer means the console. */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} sink_t;

static void emit(sink_t *s, char c) {
    if (!s->buf) { kputc(c); return; }
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}

static void emit_str(sink_t *s, const char *p) { while (*p) emit(s, *p++); }
static void emit_pad(sink_t *s, char c, int n) { while (n-- > 0) emit(s, c); }

static void put_uint(sink_t *s, u32 v, u32 base, int upper,
                     int width, char pad, bool left) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[36];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v) { buf[n++] = digits[v % base]; v /= base; }
    int digits_len = n;
    if (!left) emit_pad(s, pad, width - digits_len);
    while (n--) emit(s, buf[n]);
    if (left) emit_pad(s, ' ', width - digits_len);
}

/* A pointer, all of it.
 *
 * %p took a void *, cast it to u32 and printed eight hex digits, on a
 * kernel where nothing a pointer refers to fits in thirty-two bits: a
 * program lives at 0x8040000000 and the kernel's own half starts at
 * 0xFFFF800000000000. So every address in every panic, every black box
 * entry and every fault report was the bottom half of the real one, which
 * is not a rounded answer -- it is a different address, and one that
 * usually looks plausible.
 *
 * Found by a fault report that named 0x00000000 as the address a program
 * had read when the address it had read was 0xFFFF800000000000.
 */
static void put_ptr(sink_t *s, u64 v) {
    const char *digits = "0123456789abcdef";
    emit_str(s, "0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        emit(s, digits[(v >> shift) & 0xF]);
}

static void put_int(sink_t *s, i32 v, int width, char pad, bool left) {
    if (v < 0) {
        emit(s, '-');
        put_uint(s, (u32)(-v), 10, 0, width ? width - 1 : 0, pad, left);
    } else {
        put_uint(s, (u32)v, 10, 0, width, pad, left);
    }
}

static void format(sink_t *s, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(s, *fmt); continue; }
        fmt++;
        char pad = ' ';
        int width = 0;
        bool left = false;
        if (*fmt == '-') { left = true; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        switch (*fmt) {
            case 'd': case 'i': put_int(s, va_arg(ap, i32), width, pad, left); break;
            case 'u': put_uint(s, va_arg(ap, u32), 10, 0, width, pad, left); break;
            case 'x': put_uint(s, va_arg(ap, u32), 16, 0, width, pad, left); break;
            case 'X': put_uint(s, va_arg(ap, u32), 16, 1, width, pad, left); break;
            case 'b': put_uint(s, va_arg(ap, u32), 2, 0, width, pad, left); break;
            case 'p': put_ptr(s, (u64)(uintptr_t)va_arg(ap, void *)); break;
            case 'c': {
                char c = (char)va_arg(ap, int);
                if (!left) emit_pad(s, ' ', width - 1);
                emit(s, c);
                if (left) emit_pad(s, ' ', width - 1);
                break;
            }
            case 's': {
                const char *str = va_arg(ap, const char *);
                str = str ? str : "(null)";
                int n = (int)strlen(str);
                if (!left) emit_pad(s, ' ', width - n);
                emit_str(s, str);
                if (left) emit_pad(s, ' ', width - n);
                break;
            }
            case '%': emit(s, '%'); break;
            default: emit(s, '%'); emit(s, *fmt); break;
        }
    }
}

void kvprintf(const char *fmt, va_list ap) {
    sink_t s = { 0, 0, 0 };
    format(&s, fmt, ap);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

int kvformat(char *buf, size_t cap, const char *fmt, va_list ap) {
    if (!buf || cap == 0) return 0;
    sink_t s = { buf, cap, 0 };
    format(&s, fmt, ap);
    buf[s.len < cap ? s.len : cap - 1] = 0;
    return (int)s.len;
}

int kformat(char *buf, size_t cap, const char *fmt, ...) {
    if (!buf || cap == 0) return 0;
    sink_t s = { buf, cap, 0 };
    va_list ap;
    va_start(ap, fmt);
    format(&s, fmt, ap);
    va_end(ap);
    buf[s.len < cap ? s.len : cap - 1] = 0;
    return (int)s.len;
}

void panic(const char *fmt, ...) {
    cli();
    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n*** KERNEL PANIC ***\n");

    /* The same words go into the black box, so the reason sits in the log
       next to the phase it happened in, rather than only on a screen that
       may not exist. */
    char why[256];
    va_list ap;
    va_start(ap, fmt);
    kvformat(why, sizeof(why), fmt, ap);
    va_end(ap);
    kputs(why);
    bb_log("!! panic: %s", why);

    /* Disk before screen. Painting needs the framebuffer to be in a sane
       state, and if it is not then this faults again; by then the write has
       already happened and the log survives either way. */
    bool written = bb_flush();
    bb_screen();
    if (!written) kputs("\n(black box not written: not a zelr volume)\n");

    for (;;) hlt();
}
