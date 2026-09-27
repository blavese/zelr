#pragma once
#include "types.h"
void timer_init(u32 hz);
u64  timer_ticks(void);
u32  timer_hz(void);

/* One tick of the 8254, counted by the interrupt dispatcher before anything
   that might wait for the kernel lock. */
void timer_count_tick(void);
void sleep_ms(u32 ms);
