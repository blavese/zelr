#pragma once
#include "types.h"

/* The other half of the APIC pair.
 *
 * The 8259 is a 1976 part with eight inputs, cascaded to reach fifteen, and
 * it delivers everything to one processor. Its replacement has twenty four
 * inputs, a table saying which vector each one raises and which processor
 * gets it, and no fixed idea of which device is on which line.
 *
 * That last part is the reason this is not optional on a modern machine.
 * The 8259 is emulated by the chipset rather than present, and how faithfully
 * varies; a laptop that boots with it is running a compatibility path its
 * firmware would rather it did not. The IOAPIC is the real hardware.
 *
 * The awkwardness is that the legacy IRQ numbers do not survive. IRQ 1 being
 * the keyboard was a wiring fact of the IBM PC, and here it is whatever
 * input the board designer connected it to. The firmware lists the ones that
 * moved, and almost every machine moves the timer, so applying that list is
 * the difference between a working clock and a kernel waiting forever. */

/* Finds and maps whatever the firmware described, and masks every input, so
   nothing is delivered until something asks for it. */
bool ioapic_init(void);
bool ioapic_active(void);

/* Points a legacy IRQ at a vector on the boot processor, applying whatever
   override the firmware listed for it, and unmasks it. */
bool ioapic_route_irq(u8 irq, u8 vector);

void ioapic_mask_irq(u8 irq);
void ioapic_unmask_irq(u8 irq);

/* Which line a legacy IRQ actually arrives on, after overrides. */
u32  ioapic_gsi_for_irq(u8 irq);

u32  ioapic_inputs(void);       /* how many, across every controller found */
