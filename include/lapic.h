#pragma once
#include "types.h"

/* The processor's own interrupt controller.
 *
 * Every CPU has one, and it is the thing that actually delivers an interrupt
 * to that CPU. It is needed for two separate reasons: the startup signals
 * that bring the other processors up are sent through it, and once
 * interrupts arrive through an IOAPIC rather than the 8259 pair, it is what
 * they are acknowledged to.
 *
 * It lives here rather than inside smp.c because those two reasons are
 * independent. A machine with one processor still wants an IOAPIC, and
 * would otherwise only get a local APIC as a side effect of asking about
 * processors it does not have. */

bool lapic_init(void);
bool lapic_present(void);
u8   lapic_id(void);

/* Enables the calling processor's own local APIC. lapic_init does this for
   the one that calls it; every other processor has to do it for itself. */
void lapic_enable(void);

/* End of interrupt. Every interrupt delivered through the local APIC has to
   be acknowledged or nothing at the same or lower priority arrives again. */
void lapic_eoi(void);

/* --- a clock of its own ---------------------------------------------------
 *
 * The 8254 sends its tick to one processor. That was enough while the other
 * processors only ever ran a function handed to them and went back to sleep,
 * and it is not enough for one that is running a task: without an interrupt
 * of its own, whatever it picked up would run until it gave the processor
 * back, and a program that never does would own that processor forever.
 *
 * So each one arms its own. The count is worked out against the 8254, which
 * is the only clock this machine knows the rate of -- the local APIC counts
 * at the bus frequency, and nothing says what that is. */
void lapic_timer_calibrate(void);
void lapic_timer_start(u8 vector);
u32  lapic_timer_hz(void);

/* For smp.c, which sends startup signals through registers this does not
   otherwise need to expose. Null before lapic_init succeeds. */
volatile u8 *lapic_regs(void);
