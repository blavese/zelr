#pragma once
#include "types.h"

static inline void outb(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline u8 inb(u16 port) {
    u8 r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outw(u16 port, u16 val) {
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}
static inline u16 inw(u16 port) {
    u16 r; __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outl(u16 port, u32 val) {
    __asm__ volatile ("outl %0, %1" :: "a"(val), "Nd"(port));
}

static inline u32 inl(u16 port) {
    u32 r; __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port)); return r;
}

/* Short delay by writing to an unused port; some old PICs need it. */
static inline void io_wait(void) { outb(0x80, 0); }

static inline bool interrupts_enabled(void) {
    /* The flags register is 64 bits wide here, and pushing it pushes eight
       bytes, so popping into anything narrower does not assemble. */
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

static inline u64 rdmsr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 msr, u64 value) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((u32)value),
                                 "d"((u32)(value >> 32)));
}
static inline void cpuid_read(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                              : "a"(leaf), "c"(0));
}
static inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static inline void cli(void) { __asm__ volatile ("cli"); }
static inline void sti(void) { __asm__ volatile ("sti"); }
static inline void hlt(void) { __asm__ volatile ("hlt"); }
