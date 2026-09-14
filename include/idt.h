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
void register_interrupt_handler(u8 n, isr_handler_t h);

/* Whether anything is listening on this vector. Used to decide which lines
   are worth routing through the IOAPIC: one pointing at a vector with no
   handler is an interrupt nothing will acknowledge. */
bool idt_has_handler(u8 n);
