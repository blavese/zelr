/* Programmable Interval Timer on IRQ0. This is the heartbeat the scheduler
   preempts on. */
#include "timer.h"
#include "ps2.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "usb.h"

static volatile u64 ticks = 0;
static u32 frequency = 100;

static void on_tick(registers_t *r) {
    (void)r;
    ticks++;

    /* The USB controller is not wired to an interrupt here, so this is what
       moves it along: a key pressed on a USB keyboard is noticed on the next
       tick. See kernel/xhci.c for why it is polled rather than wired up. */
    usb_poll();

    /* And the 8042, which does raise an interrupt but only on the edge of a
       byte arriving. One that turned up while the line was still masked, or
       while an interrupt was lost, sits in the output buffer and stops the
       controller delivering anything further. Before this, that was a
       keyboard and a mouse dead until the machine was power cycled. */
    ps2_poll_from_timer();
}

void timer_init(u32 hz) {
    frequency = hz;
    u32 divisor = 1193182u / hz;
    outb(0x43, 0x36);                       /* channel 0, lo/hi, square wave */
    outb(0x40, (u8)(divisor & 0xFF));
    outb(0x40, (u8)((divisor >> 8) & 0xFF));
    register_interrupt_handler(32, on_tick);
    pic_unmask(0);
}

u64 timer_ticks(void) { return ticks; }
u32 timer_hz(void)    { return frequency; }

void sleep_ms(u32 ms) {
    u32 delta = (ms * frequency) / 1000u;
    u64 target = ticks + delta;
    while (ticks < target) __asm__ volatile ("hlt");
}
