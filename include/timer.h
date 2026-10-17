#pragma once
#include "types.h"
void timer_init(u32 hz);
u64  timer_ticks(void);
u32  timer_hz(void);

/* One tick of the 8254, counted by the interrupt dispatcher before anything
   that might wait for the kernel lock. */
void timer_count_tick(void);
void sleep_ms(u32 ms);

/* A deadline that passes with interrupts off, for a wait inside a system
   call, during boot or on the panic path, where timer_ticks does not move.
   Started for so many milliseconds (or microseconds); asked in the loop that
   waits. */
typedef struct { u32 left, still; u16 last; } spin_deadline_t;
void spin_deadline_start(spin_deadline_t *d, u32 ms);
void spin_deadline_start_us(spin_deadline_t *d, u32 us);
bool spin_deadline_passed(spin_deadline_t *d);
