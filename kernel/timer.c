/* Programmable Interval Timer on IRQ0. This is the heartbeat the scheduler
   preempts on. */
#include "timer.h"
#include "ps2.h"
#include "sound.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "usb.h"
#include "synaptics.h"
#include "rng.h"

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

    /* And the release of a trackpad tap, which nothing else can cause: by
       the time it has to happen the finger is already off the pad. */
    syn_tick();

    /* And the sound buffer, which plays on a loop and would otherwise repeat
       whatever was last written to it forever. */
    sound_poll();

    /* One cycle counter sample. This is the only place the interval between
       two interrupts can be measured, and on a machine with no rdseed and no
       rdrand it is the only unpredictable thing there is. */
    rng_tick();
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

/* Asked for milliseconds, delivered in ticks, and never fewer than one.
 *
 * The timer runs at a hundred hertz, so anything under ten milliseconds used
 * to divide to zero ticks and this returned without waiting for anything.
 * That is not a short sleep; it is a busy loop wearing the word sleep, and
 * every caller that used it to pace something got no pacing at all.
 *
 * What it cost: the sound buffer's writer waits for room by sleeping two
 * milliseconds at a time and gives up when two hundred and fifty of those
 * pass without the hardware moving. At zero milliseconds each that is a few
 * microseconds, so any note longer than the third of a second the buffer
 * holds was abandoned a third of the way through -- silently, because
 * nothing checks how much of a note was accepted. A one second note played
 * for a third of a second and the machine reported nothing wrong.
 *
 * Rounded up rather than down, for the same reason: a sleep that is asked
 * for and not taken is worse than one that is a few milliseconds long. */
void sleep_ms(u32 ms) {
    if (!ms) return;
    u32 delta = (ms * frequency + 999u) / 1000u;
    if (!delta) delta = 1;
    u64 target = ticks + delta;
    while (ticks < target) __asm__ volatile ("hlt");
}
