#pragma once
#include "types.h"

/* The other processors.
 *
 * A PC starts with one CPU running and the rest halted. Bringing them up is
 * a protocol: find them through ACPI, send each one an INIT signal followed
 * by a startup signal carrying the page number to begin executing at, and
 * wait for it to report in.
 *
 * What they do afterwards used to be the open question, and the answer used
 * to be: wait for a function to be handed to you, run it, go back to sleep.
 * That is real parallelism with a small surface and it is not a processor
 * running anything -- a machine given four cores ran every program on one.
 *
 * They run programs now. Each has a task state segment of its own, a
 * current task of its own and a timer of its own, and one lock covers the
 * kernel: a processor holds it whenever it is not executing ring 3 code.
 * That is the coarsest lock there is, and it is the honest one to start
 * with -- the alternative is a lock on the heap, the task list, the
 * filesystem and every driver, which is not one change but forty, and the
 * first wrong one is a machine that corrupts itself occasionally.
 *
 * Kernel tasks stay on the boot processor. What the others take is programs.
 *
 * The handing out of functions is still here, because the compositor uses it
 * for half of every frame comparison, and what it hands over is arithmetic
 * over memory the caller owns rather than anything the kernel keeps. */

#define SMP_MAX_CPUS 16

typedef struct {
    u8   apic_id;
    bool started;
    volatile u32 jobs;          /* work items finished */
    volatile u64 spins;         /* how many times it looked for work */

    /* Slices this processor has given to a program. The one number that
       says whether a processor is running anything: jobs are functions
       handed to it by another processor, and a processor that has never
       run a program has never been a processor as far as anybody using
       this machine is concerned. */
    volatile u64 user_slices;

    /* Its own timer, counted where it lands. A processor that never
       gets one cannot be taken off whatever it picked up, and is a
       processor that looks asleep for a reason nothing else shows. */
    volatile u64 local_ticks;
    volatile u64 lock_misses;   /* ticks that found the kernel busy */
} cpu_t;

void smp_init(void);

/* Which processor is asking, as an index into the table below.
 *
 * By its local APIC id, looked up rather than remembered: there is nowhere
 * to remember it that is per processor until this exists, which is the
 * shape of every chicken and egg in a kernel. The table is at most sixteen
 * entries and the comparison is a byte, so the walk costs less than the
 * machinery to avoid it.
 *
 * Zero before the local APIC is up, which is the boot processor, and is the
 * right answer then. */
u32  smp_this_cpu(void);

/* Counted where the interrupt lands. */
void smp_note_tick(u32 cpu);
void smp_note_lock_miss(u32 cpu);

u32  smp_cpu_count(void);       /* processors the firmware described */
u32  smp_started(void);         /* how many actually came up, boot one included */
const cpu_t *smp_cpu(u32 i);
bool smp_active(void);

/* Hands a function to another processor. Returns false if that one is not
   running or is still busy. The function runs with interrupts off. */
bool smp_run(u32 cpu, void (*fn)(void *), void *arg);
bool smp_busy(u32 cpu);

/* Whether a function is waiting to be run on that processor. The scheduler
   asks, so that a processor which has been handed a piece of a frame is not
   given a program to run instead: the compositor is waiting on it, and a
   frame's worth of spinning is a visible stall. */
bool smp_work_pending(u32 cpu);

/* A processor other than this one that is up and idle, or zero. */
u32  smp_helper(void);
bool smp_wait(u32 cpu, u32 timeout_ms);

/* A plain spinlock, for anything two processors might touch at once. */
typedef volatile u32 spinlock_t;
void spin_lock(spinlock_t *lock);

/* Takes it if it is free and says so, rather than waiting. For a caller
   that has something better to do than wait, and for one that must not
   wait at all. */
bool spin_try(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
