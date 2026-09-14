#pragma once
#include "types.h"
void pic_init(void);
void pic_eoi(u8 irq);
void pic_mask(u8 irq);
void pic_unmask(u8 irq);

/* Masks every line. Used when an IOAPIC takes over: the firmware says
   whether the pair is wired through, and if it is, leaving it enabled means
   the same device delivers an interrupt twice, once down each path. */
void pic_disable(void);
