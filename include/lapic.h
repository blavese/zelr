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

/* End of interrupt. Every interrupt delivered through the local APIC has to
   be acknowledged or nothing at the same or lower priority arrives again. */
void lapic_eoi(void);

/* For smp.c, which sends startup signals through registers this does not
   otherwise need to expose. Null before lapic_init succeeds. */
volatile u8 *lapic_regs(void);
