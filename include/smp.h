#pragma once
#include "types.h"

/* The other processors.
 *
 * A PC starts with one CPU running and the rest halted. Bringing them up is
 * a protocol: find them through ACPI, send each one an INIT signal followed
 * by a startup signal carrying the page number to begin executing at, and
 * wait for it to report in.
 *
 * What they do afterwards is the open question. Sharing the scheduler would
 * mean a lock on every structure the kernel has, so instead each processor
 * waits for work to be handed to it and runs it. That is real parallelism
 * with a small surface: the boot processor owns the kernel, and the others
 * own nothing until they are given something. */

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
