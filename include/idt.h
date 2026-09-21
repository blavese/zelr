#pragma once
#include "types.h"

/* What an interrupt leaves on the stack, lowest address first.
 *
 * The order is the reverse of the order isr.S pushes in, because a push moves
 * downward. Long mode always pushes rsp and ss, even when the privilege level
 * did not change, which 32-bit did not, so there is no special case for a
 * frame taken in the kernel. */
typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 int_no, err_code;
    u64 rip, cs, rflags, rsp, ss;
} registers_t;

typedef void (*isr_handler_t)(registers_t *);

void idt_init(void);

/* Loads the table built by idt_init on the processor that calls it. There is
   one table and every processor shares it; each still has to point its own
   register at it. */
void idt_load(void);

/* The vector one processor sends to another to wake it from a halt. Above
   the hardware lines and below the system call gate, so it collides with
   neither. */
#define VEC_AP_WAKE 0xF0

/* And the one a task raises on itself to be switched away from.
 *
 * This used to be the timer's vector, because the timer's handler ends in
 * the scheduler and raising it got the switch for free. What it also got was
 * the rest of that handler -- the tick counter, the polling, the sound
 * buffer -- all of it run as though a hundredth of a second had passed when
 * nothing of the sort had happened.
 *
 * The cost was the clock. Every yield added a tick, so the number of ticks
 * in a second depended on how often the machine happened to give up a slice:
 * a hundred hertz timer read as two hundred with one task sleeping in a
 * loop, and every sleep, timeout and deadline in the kernel is counted in
 * those ticks, so all of them were short together and none of them said so. */
#define VEC_YIELD   0xF1

/* --- a processor's own clock ---------------------------------------------
 *
 * The 8254 sends its tick to one processor, and vector 32 is that tick: it
 * counts time for the machine. A processor running a program needs an
 * interrupt of its own to be taken off it, and that is this one -- the same
 * switch, on a different processor, without a second claim about what time
 * it is. */
#define VEC_LOCAL_TIMER 0xF2
void register_interrupt_handler(u8 n, isr_handler_t h);

/* Whether anything is listening on this vector. Used to decide which lines
   are worth routing through the IOAPIC: one pointing at a vector with no
   handler is an interrupt nothing will acknowledge. */
bool idt_has_handler(u8 n);
