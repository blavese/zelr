#pragma once
#include "types.h"

/* Selectors, which are byte offsets into the descriptor table rather than
   indices. The low two bits carry the requested privilege level, so a user
   selector is the offset with 3 added. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20

/* --- one task state segment per processor --------------------------------
 *
 * There was one, and one was enough while the other processors only ever ran
 * a function handed to them with interrupts off. It is not enough for a
 * processor running a program: the TSS is where the processor finds the
 * stack to switch to when an interrupt arrives from ring 3, and two
 * processors sharing one would take their interrupts onto the same kernel
 * stack at the same time. That is not a fault that shows up later in
 * something unrelated -- it is two interrupt frames written over each other
 * as they are pushed.
 *
 * So the table carries one descriptor per processor, two slots each because
 * a TSS descriptor is the one thing in long mode that still needs a 64-bit
 * base, and each processor loads its own. The descriptors themselves are
 * shared and read only; what is not shared is the segment each one points
 * at. */
#define GDT_TSS(cpu)    (0x28 + (u16)(cpu) * 16)
#define GDT_TSS_FIRST   GDT_TSS(0)

#define USER_CODE_SEL (GDT_USER_CODE | 3)
#define USER_DATA_SEL (GDT_USER_DATA | 3)

void gdt_init(void);

/* What a processor other than the boot one calls for itself: the same table,
   and the task register pointing at its own segment. */
void gdt_load_cpu(u32 cpu);

/* The stack an interrupt taken in ring 3 lands on, for the processor that
   calls it. */
void tss_set_stack(u64 rsp0);

/* And for one named rather than the caller, which is what setting a
   processor up before it runs anything needs. */
void tss_set_stack_for(u32 cpu, u64 rsp0);

/* Which segment the calling processor's task register actually holds. Asked
   of the processor rather than remembered, because the thing worth checking
   is what it is using and not what it was told. */
u16  tss_current_selector(void);

/* What one processor's segment says its ring 0 stack is. For the self test,
   which has to be able to tell two segments apart by something other than
   their selector. */
u64  tss_stack_of(u32 cpu);
