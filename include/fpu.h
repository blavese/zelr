#pragma once
#include "types.h"

/* The floating point unit, and the registers that come with it.
 *
 * A machine starts with SSE turned off. Not absent: the registers are there,
 * and the processor faults on the first instruction that touches one until
 * something says it is prepared to look after them. Nothing here ever did,
 * so every part of this project was compiled with the floating point
 * instructions switched off, and the whole system was integers.
 *
 * That was survivable for a kernel. It is not survivable for a language: a
 * number in JavaScript is a double, and one that quietly truncated would be
 * wrong about money, wrong about time, and wrong in a way no test written
 * against integers would ever catch.
 *
 * What "prepared to look after them" means is the hard part. There are
 * sixteen 128 bit registers and a control word, and they belong to whichever
 * task is running; two tasks doing arithmetic at the same time will read
 * each other's operands unless the whole set is put away on every switch.
 * FXSAVE writes all of it to 512 bytes, which must be 16 byte aligned or the
 * instruction faults.
 *
 * The kernel itself stays compiled without SSE. That is deliberate and it is
 * what every kernel does: an interrupt can land between any two instructions
 * of a program, and if the handler used a vector register it would have to
 * save the set before touching one. Keeping the kernel integer-only means
 * the registers belong to user code alone and the only place they move is a
 * task switch.
 */

#define FPU_AREA 512

/* Turns SSE on for the processor this runs on. Called once on the boot
   processor and once on each of the others, because the control registers
   are per processor and an application processor that missed this faults on
   the first user program it is handed. */
void fpu_init(void);

/* True once fpu_init has run here, for anything that wants to report it. */
bool fpu_ready(void);

/* The state of the registers, into and out of a 16 byte aligned 512 byte
   area. The scheduler owns the areas; these only move the bytes. */
void fpu_save(void *area);
void fpu_restore(const void *area);

/* A freshly initialised state, for a task that has not run yet: the control
   word set the way the language expects and every register zero. Written
   into the area rather than loaded, so making a task costs no instructions
   on the processor's floating point side at all. */
void fpu_blank(void *area);
