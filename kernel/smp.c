/* Starting the other processors.
 *
 * The sequence is fixed by the architecture and there is no shortcut. Each
 * application processor is sent an INIT inter-processor interrupt, which
 * resets it, and then a startup interrupt carrying a page number. It begins
 * executing there in 16-bit real mode with nothing set up, which is what
 * bootloader/trampoline.S is for.
 *
 * The trampoline has to live below a megabyte on a page boundary, because
 * the startup signal carries a page number in one byte. It is copied to
 * 0x8000 at runtime, and the parameters it needs are written into it just
 * before each processor is started.
 *
 * What happens after that is a design decision rather than a requirement.
 * Letting the other processors run the scheduler would mean a lock on the
 * heap, the task list, the filesystem and every driver, and getting one of
 * those wrong produces a fault that happens once an hour on one machine.
 * Instead each of them sits in a loop waiting to be handed a function. The
 * boot processor still owns the kernel; the others own nothing until they
 * are given something, so there is nothing to race over.
 */
#include "smp.h"
#include "idt.h"
#include "acpi.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "timer.h"
#include "string.h"
#include "printf.h"
#include "io.h"
#include "lapic.h"

extern const u8 trampoline_start[], trampoline_end[];

#define TRAMPOLINE_PHYS 0x8000
#define AP_STACK_SIZE   16384

/* Local APIC registers, as offsets from the base. */
#define LAPIC_ID     0x020
#define LAPIC_VER    0x030
#define LAPIC_SVR    0x0F0
#define LAPIC_ICR_LO 0x300
#define LAPIC_ICR_HI 0x310

#define ICR_INIT       0x00000500
#define ICR_STARTUP    0x00000600
#define ICR_ASSERT     0x00004000
#define ICR_LEVEL      0x00008000
#define ICR_PENDING    0x00001000

typedef struct {
    cpu_t info;
    void (*volatile fn)(void *);
    void *volatile arg;
} slot_t;

static slot_t cpus[SMP_MAX_CPUS];
static u32 ncpus;              /* entries in `cpus`, boot processor first */
static u32 nstarted = 1;       /* the one already running counts */
static volatile u8 *lapic;
static bool active;

u32 smp_cpu_count(void) { return ncpus; }
u32 smp_started(void) { return nstarted; }
bool smp_active(void) { return active; }

const cpu_t *smp_cpu(u32 i) {
    return i < ncpus ? &cpus[i].info : 0;
}

/* --- locking ------------------------------------------------------------ */

void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(lock, 1))
        while (*lock) __asm__ volatile ("pause");
}

void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(lock);
}

/* --- the local APIC ----------------------------------------------------- */

static void apic_write(u32 reg, u32 value) {
    *(volatile u32 *)(lapic + reg) = value;
}

static u32 apic_read(u32 reg) {
    return *(volatile u32 *)(lapic + reg);
}

static void apic_wait(void) {
    for (u32 i = 0; i < 1000000; i++)
        if (!(apic_read(LAPIC_ICR_LO) & ICR_PENDING)) return;
}

/* A short delay. The timer runs at 100 Hz, which is far too coarse for the
   microsecond waits the startup sequence asks for, so this reads the PIT's
   counter directly. */
static void delay_us(u32 us) {
    /* Port 0x61 bit 4 toggles every 15.085 microseconds on every PC, which
       is the one timing source available before anything is set up. */
    for (u32 i = 0; i < us / 15 + 1; i++) {
        u8 start = inb(0x61) & 0x10;
        u32 guard = 0;
        while ((inb(0x61) & 0x10) == start && guard++ < 100000) { }
    }
}

/* --- bringing one up ---------------------------------------------------- */

static void ap_main(void *arg);

/* Writes the parameters into the copy of the trampoline at 0x8000. */
static bool patch_trampoline(u64 stack_top, u64 index) {
    u8 *code = (u8 *)TRAMPOLINE_PHYS;
    u64 len = (u64)(trampoline_end - trampoline_start);

    for (u64 i = 0; i + 8 + 32 <= len; i += 4) {
        if (memcmp(code + i, "ZELRSMP1", 8) != 0) continue;
        /* Four addresses, each a full machine word now: the page tables, the
           stack, where to go and which processor this is. */
        u64 *p = (u64 *)(code + i + 8);
        p[0] = paging_kernel_directory();
        p[1] = stack_top;
        p[2] = (u64)ap_main;
        p[3] = index;
        return true;
    }
    return false;
}

static bool start_cpu(u32 index) {
    u8 apic_id = cpus[index].info.apic_id;

    u8 *stack = (u8 *)kmalloc(AP_STACK_SIZE);
    if (!stack) return false;
    memset(stack, 0, AP_STACK_SIZE);
    u64 top = ((u64)stack + AP_STACK_SIZE) & ~0xFull;

    if (!patch_trampoline(top, index)) { kfree(stack); return false; }

    /* INIT: assert, then deassert, then let it settle. */
    apic_write(LAPIC_ICR_HI, (u32)apic_id << 24);
    apic_write(LAPIC_ICR_LO, ICR_INIT | ICR_ASSERT | ICR_LEVEL);
    apic_wait();
    apic_write(LAPIC_ICR_HI, (u32)apic_id << 24);
    apic_write(LAPIC_ICR_LO, ICR_INIT | ICR_LEVEL);
    apic_wait();
    delay_us(10000);

    /* Startup, twice. The specification says to send it a second time if the
       processor has not reported in, and sending it regardless is what every
       real implementation does because the first one is occasionally lost. */
    for (int attempt = 0; attempt < 2; attempt++) {
        apic_write(LAPIC_ICR_HI, (u32)apic_id << 24);
        apic_write(LAPIC_ICR_LO, ICR_STARTUP | (TRAMPOLINE_PHYS >> 12));
        apic_wait();
        delay_us(200);

        for (u32 spin = 0; spin < 200; spin++) {
            if (cpus[index].info.started) return true;
            delay_us(1000);
        }
    }

    return cpus[index].info.started;
}

/* Where an application processor arrives, with paging on and a stack of its
   own. It never returns.
 *
 * It used to sit in a tight loop reading its own work slot, on the grounds
 * that it had no interrupt table and did not need one. That loop is why
 * zelr would not boot on a machine given more than one processor: it holds
 * a core at a hundred percent for as long as the machine is on, and under
 * a hypervisor that is enough to starve the boot processor of the time it
 * needs to take a timer interrupt. The symptom is not a slow machine, it
 * is a dead one: the scheduler waits for a first tick that never comes, so
 * nothing runs, and a keyboard nothing is reading looks broken.
 *
 * It sleeps now, and is woken when there is something to do. That costs a
 * shared interrupt table, a vector and an inter-processor interrupt, and
 * it is what an idle processor is supposed to do on any machine.
 */
static void ap_main(void *arg) {
    u64 index = (u64)arg;
    if (index >= SMP_MAX_CPUS) for (;;) __asm__ volatile ("cli; hlt");

    slot_t *me = &cpus[index];

    /* Its own local APIC first: the registers are already mapped, but the
       switch that lets this processor receive anything is per processor and
       has only ever been thrown on the boot one. */
    lapic_enable();

    /* The table is the one the boot processor built; this points at it. A
       processor that halts with interrupts on and no table would triple
       fault on the first one that arrived. */
    idt_load();

    me->info.started = true;

    for (;;) {
        /* Checked with interrupts off, so a wake-up cannot arrive between
           finding no work and going to sleep and be lost. sti does not take
           effect until after the instruction that follows it, which is what
           makes the halt below safe. */
        __asm__ volatile ("cli");
        void (*fn)(void *) = me->fn;
        if (fn) {
            __asm__ volatile ("sti");
            void *a = me->arg;
            me->fn = 0;
            fn(a);
            me->info.jobs++;
            continue;
        }
        me->info.spins++;
        __asm__ volatile ("sti; hlt");
    }
}

/* --- handing out work --------------------------------------------------- */

bool smp_busy(u32 cpu) {
    if (cpu == 0 || cpu >= ncpus) return false;
    return cpus[cpu].fn != 0;
}

bool smp_run(u32 cpu, void (*fn)(void *), void *arg) {
    if (cpu == 0 || cpu >= ncpus) return false;
    if (!cpus[cpu].info.started || cpus[cpu].fn) return false;

    cpus[cpu].arg = arg;
    /* The function pointer is written last, because it is what the other
       processor tests. Writing it first would let it read a stale argument. */
    __sync_synchronize();
    cpus[cpu].fn = fn;

    /* It is asleep until told otherwise. */
    apic_write(LAPIC_ICR_HI, (u32)cpus[cpu].info.apic_id << 24);
    /* Asserted, edge triggered. Everything except an INIT de-assert has to
       say so; sent without it the interrupt is simply not delivered, and a
       processor waiting to be woken waits forever. */
    apic_write(LAPIC_ICR_LO, ICR_ASSERT | VEC_AP_WAKE);
    apic_wait();
    return true;
}

bool smp_wait(u32 cpu, u32 timeout_ms) {
    if (cpu == 0 || cpu >= ncpus) return false;
    u64 deadline = timer_ticks() + (timeout_ms * timer_hz()) / 1000u + 1;
    while (cpus[cpu].fn) {
        if (timer_ticks() > deadline) return false;
        __asm__ volatile ("pause");
    }
    return true;
}

/* --- bringing them all up ----------------------------------------------- */

void smp_init(void) {
    memset(cpus, 0, sizeof(cpus));
    ncpus = 1;
    nstarted = 1;
    active = false;

    if (!acpi_init()) return;
    const acpi_info_t *a = acpi();
    if (a->ncpus == 0) return;

    /* Mapped and enabled by lapic.c, which may already have been asked for
       it by the interrupt routing. Doing it in two places would mean two
       mappings of the same registers and two opinions about whether it is
       switched on. */
    if (!lapic_init()) return;
    lapic = lapic_regs();
    if (!lapic) return;

    u8 self = lapic_id();
    cpus[0].info.apic_id = self;
    cpus[0].info.started = true;

    for (u32 i = 0; i < a->ncpus && ncpus < SMP_MAX_CPUS; i++) {
        if (a->apic_id[i] == self) continue;
        if (!a->usable[i]) continue;
        cpus[ncpus].info.apic_id = a->apic_id[i];
        ncpus++;
    }

    if (ncpus < 2) { active = true; return; }

    /* The trampoline has to be somewhere a processor coming out of reset can
       reach, which means below a megabyte and on a page boundary. */
    memcpy((void *)TRAMPOLINE_PHYS, trampoline_start,
           (u32)(trampoline_end - trampoline_start));

    for (u32 i = 1; i < ncpus; i++)
        if (start_cpu(i)) nstarted++;

    active = true;
}
