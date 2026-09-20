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
} cpu_t;

void smp_init(void);

u32  smp_cpu_count(void);       /* processors the firmware described */
u32  smp_started(void);         /* how many actually came up, boot one included */
const cpu_t *smp_cpu(u32 i);
bool smp_active(void);

/* Hands a function to another processor. Returns false if that one is not
   running or is still busy. The function runs with interrupts off. */
bool smp_run(u32 cpu, void (*fn)(void *), void *arg);
bool smp_busy(u32 cpu);

/* A processor other than this one that is up and idle, or zero. */
u32  smp_helper(void);
bool smp_wait(u32 cpu, u32 timeout_ms);

/* A plain spinlock, for anything two processors might touch at once. */
typedef volatile u32 spinlock_t;
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
