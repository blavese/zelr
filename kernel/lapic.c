/* See include/lapic.h. */
#include "lapic.h"
#include "timer.h"
#include "io.h"
#include "acpi.h"
#include "paging.h"

#define LAPIC_ID   0x020
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

/* Periodic, and divided by sixteen so the count fits comfortably in a
   thirty two bit register at any bus speed a PC has. */
#define LVT_PERIODIC 0x20000
#define LVT_MASKED   0x10000
#define DIV_16       0x3
#define LAPIC_SVR  0x0F0          /* spurious interrupt vector register */
#define LAPIC_EOI  0x0B0

static volatile u8 *regs;

volatile u8 *lapic_regs(void) { return regs; }
bool lapic_present(void)      { return regs != 0; }

static void write(u32 reg, u32 value) { *(volatile u32 *)(regs + reg) = value; }
static u32  read(u32 reg)             { return *(volatile u32 *)(regs + reg); }

bool lapic_init(void) {
    if (regs) return true;                  /* already up */

    /* The tables say where it is. There is an architectural default and a
       machine register that also holds the address, but the firmware is
       allowed to move it and the tables are where it says so. */
    if (!acpi_init()) return false;
    const acpi_info_t *a = acpi();
    if (!a->lapic_base) return false;

    /* Above the identity mapped region on every real machine. */
    regs = (volatile u8 *)paging_map_device(a->lapic_base & ~0xFFFull, 0x1000);
    if (!regs) return false;

    /* Bit 8 of the spurious vector register is what switches the thing on.
       The low eight bits are the vector delivered for an interrupt that was
       withdrawn before it could be taken; 0xFF is the conventional choice
       and nothing is registered for it, so it is ignored if it ever fires. */
    write(LAPIC_SVR, read(LAPIC_SVR) | 0x100 | 0xFF);
    return true;
}

/* Switches on the local APIC of the processor that calls it.
 *
 * lapic_init maps the registers and returns early once they are mapped, so
 * the enable bit it writes is written on the boot processor and nowhere
 * else. The address is shared but the register is not: every processor has
 * its own, and one that has never been enabled delivers nothing at all, not
 * even an interrupt sent to it by name. */
void lapic_enable(void) {
    if (!regs) return;
    write(LAPIC_SVR, read(LAPIC_SVR) | 0x100 | 0xFF);
}

u8 lapic_id(void) {
    return regs ? (u8)(read(LAPIC_ID) >> 24) : 0;
}

/* --- the processor's own clock -------------------------------------------
 *
 * The local APIC counts down at the bus frequency, and nothing tells a
 * program what that is. So it is measured against the 8254, which is the
 * one clock on this machine whose rate is known: let the counter run for a
 * known number of 8254 ticks and see how far it got.
 *
 * Measured once, on the boot processor, and used by all of them. The bus
 * clock is the bus clock; a machine where one processor counted at a
 * different rate from another would have worse problems than this.
 */
static u32 ticks_per_second;

u32 lapic_timer_hz(void) { return ticks_per_second; }

/* The 8254's own counter, latched and read.
 *
 * Channel 0 is the one driving the timer interrupt. It counts down from
 * whatever timer_init programmed at 1.193182 MHz and reloads, so watching
 * it wrap is a clock that does not need an interrupt -- which matters,
 * because this runs during boot with the interrupt flag still clear.
 *
 * The first version waited on timer_ticks(), which is incremented by that
 * interrupt. It never moved and the machine stopped at "== smp": a
 * calibration loop is the worst place to learn that interrupts are not on
 * yet, because it does not fail, it waits. */
static u16 pit_count(void) {
    outb(0x43, 0x00);                  /* latch channel 0 */
    u8 lo = inb(0x40);
    u8 hi = inb(0x40);
    return (u16)((u16)lo | ((u16)hi << 8));
}

void lapic_timer_calibrate(void) {
    if (!regs || ticks_per_second) return;

    write(LAPIC_TIMER_DIV, DIV_16);
    write(LAPIC_LVT_TIMER, LVT_MASKED);      /* count, but deliver nothing */
    write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);

    /* Five reloads of the 8254, which at a hundred hertz is fifty
       milliseconds. Bounded, because a machine whose 8254 is not running
       must come up without a working measurement rather than not at all:
       a rate of nought simply means no processor arms one of these, which
       is where this machine was before any of it. */
    u32 want = 5, wraps = 0;
    u16 prev = pit_count();
    for (u32 guard = 0; guard < 200000000u && wraps < want; guard++) {
        u16 now = pit_count();
        if (now > prev) wraps++;       /* counts down, so up is a reload */
        prev = now;
    }

    u32 left = read(LAPIC_TIMER_CUR);
    write(LAPIC_TIMER_INIT, 0);              /* stop it again */

    if (wraps < want) { ticks_per_second = 0; return; }

    u32 counted = 0xFFFFFFFFu - left;
    u32 hz = timer_hz() ? timer_hz() : 100;
    ticks_per_second = counted * (hz / want);

    /* A number too small to divide into a tick would arm a timer that never
       fires, which is a processor that picks a program up and never puts it
       down. Nought is the honest answer and means no timer at all. */
    if (ticks_per_second < hz * 100) ticks_per_second = 0;
}

void lapic_timer_start(u8 vector) {
    if (!regs || !ticks_per_second) return;
    u32 hz = timer_hz() ? timer_hz() : 100;

    write(LAPIC_TIMER_DIV, DIV_16);
    write(LAPIC_LVT_TIMER, (u32)vector | LVT_PERIODIC);
    write(LAPIC_TIMER_INIT, ticks_per_second / hz);
}


void lapic_eoi(void) {
    if (regs) write(LAPIC_EOI, 0);
}
