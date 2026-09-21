/* Round-robin preemptive scheduler.
 *
 * Every task owns a kernel stack. A task that is not running has a complete
 * interrupt frame sitting on that stack, so switching tasks is just a matter
 * of telling the interrupt return path to unwind a different one. */
#include "sched.h"
#include "smp.h"
#include "fd.h"
#include "signal.h"
#include "winsrv.h"
#include "vfs.h"
#include "syscall.h"
#include "wait.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "gdt.h"
#include "io.h"
#include "paging.h"
#include "fpu.h"

#define STACK_SIZE TASK_STACK_SIZE

/* Every kernel stack is painted with this before anything runs on it. Two
   things come out of that.
 *
 * The first is that the bottom word is a guard: a task that runs past the
 * end of its stack writes over it, and the switch away from that task says
 * so. Without it the first sign of an overflow is somewhere else entirely.
 * A kernel stack comes from the heap, and the bytes below it are the header
 * of the neighbouring block, so overrunning one corrupts a free list that
 * some unrelated task walks into later: a fault in kmalloc, in a task that
 * did nothing wrong, long after the one that did has finished.
 *
 * The second is that the paint that is left says how close every task came,
 * which is the only honest way to pick a stack size. */
#define STACK_PAINT 0xC5C5C5C5C5C5C5C5ull

void sched_paint_stack(u64 base) {
    u64 *w = (u64 *)base;
    for (u32 i = 0; i < STACK_SIZE / 8; i++) w[i] = STACK_PAINT;
}

static void paint_stack(u8 *stack) {
    u64 *w = (u64 *)stack;
    for (u32 i = 0; i < STACK_SIZE / 8; i++) w[i] = STACK_PAINT;
}

/* Where this task's FXSAVE area actually starts. The record came off a heap
   that aligns to eight and the instruction needs sixteen, so the area is
   over-allocated and the aligned address inside it is used. */
static inline u8 *fpu_area_of(task_t *t) {
    u64 a = (u64)t->fpu;
    return (u8 *)((a + 15) & ~(u64)15);
}

/* Bytes at the bottom that have never been written. A task that has used
   all of its stack reads zero here, and one that has used more than all of
   it does not get this far. */
u32 task_stack_headroom(const task_t *t) {
    const u64 *w = (const u64 *)t->stack_base;
    u32 i = 0;
    while (i < STACK_SIZE / 8 && w[i] == STACK_PAINT) i++;
    return i * 8;
}

/* How long a finished task's record survives so its status can be collected.
   Ten seconds is far longer than any wait here takes and short enough that
   nothing accumulates. */
#define REAP_GRACE (10u * 100u)      /* in ticks, at 100 Hz */

static task_t *head;        /* circular list */

/* --- what each processor is running --------------------------------------
 *
 * There was one `current`, because there was one processor that ran tasks.
 * Everything that asks "what is running" -- a system call working out whose
 * files to look in, a fault deciding whose program to stop, the scheduler
 * itself -- was asking about the machine, and now has to ask about the
 * processor it is asking from.
 *
 * Indexed by the number smp_this_cpu gives back, which is the processor's
 * local APIC id looked up in the table smp.c keeps. */
static task_t *current_of[SMP_MAX_CPUS];

static inline task_t *cur(void)          { return current_of[smp_this_cpu()]; }
static inline void set_cur(task_t *t)    { current_of[smp_this_cpu()] = t; }

/* --- one lock for the kernel ---------------------------------------------
 *
 * See the note in sched.h for what it covers. `holder` is which processor
 * has it, and is written only by the one that holds it, so reading it is
 * asking a question whose answer cannot change under the asker: either it
 * is you, in which case nobody else can change it, or it is not, in which
 * case the answer you want is already no. */
static spinlock_t kernel_lock;
static volatile int lock_holder = -1;

void kernel_lock_acquire(void) {
    spin_lock(&kernel_lock);
    lock_holder = (int)smp_this_cpu();
}

bool kernel_lock_try(void) {
    if (!spin_try(&kernel_lock)) return false;
    lock_holder = (int)smp_this_cpu();
    return true;
}

void kernel_lock_release(void) {
    lock_holder = -1;
    spin_unlock(&kernel_lock);
}

bool kernel_lock_held_here(void) {
    return lock_holder == (int)smp_this_cpu();
}

/* --- somewhere to go when there is nothing to do -------------------------
 *
 * There was nowhere. pick_next walked the ring, found nothing runnable, and
 * returned the task it had been given -- which was the task that had just
 * asked to sleep. The switch then set it running again. So a sleep returned
 * with no time passed whenever nothing else wanted the processor, which on
 * a machine running one program is most of the time.
 *
 * Nothing about that is visible as a sleep that did not sleep. What it
 * looks like is a program that spins: the browser could not yield without
 * stopping, so it spun, and spinning starved every other task on the
 * machine. A terminal raised in front of it took neither a typed line nor
 * a resize. Typing went lossy because the console was not read often
 * enough to drain the uart. The compositor caught programs half way
 * through a frame, every frame, because no program was ever between
 * frames. Four different faults, and one of them.
 *
 * So there is a task whose whole job is to be runnable when nothing else
 * is. It halts, which is also the thing that lets the processor cool down
 * and a laptop stop spinning its fan, and it charges every tick it spends
 * there to itself as idle rather than as work. */
/* One each. A processor with nothing to run has to have somewhere of its
   own to be: sharing one idle task between two of them would be one task
   running on two processors, which is the one thing a task cannot do. */
static task_t *idle_of[SMP_MAX_CPUS];

static void idle_entry(void) {
    for (;;) {
        u64 before = timer_ticks();
        /* Interrupts are already on -- the frame this task was built with
           sets IF -- so the halt ends at the next tick at the latest. */
        hlt();
        task_t *me = cur();
        if (me) me->idle_ticks += timer_ticks() - before;
    }
}

static bool is_idle(const task_t *t) {
    for (u32 i = 0; i < SMP_MAX_CPUS; i++) if (idle_of[i] == t) return true;
    return false;
}

bool task_is_idle(const task_t *t) { return is_idle(t); }
static u32 next_pid = 1;
static bool started = false;

void sched_init(void) { head = 0; next_pid = 1; started = false; }

/* A task starts in the directory its creator was in, which is what makes
   `cd` then `exec` behave the way anyone would expect. */
static void inherit_cwd(task_t *t) {
    task_t *parent = task_current();
    if (parent && parent->cwd[0]) strncpy(t->cwd, parent->cwd, TASK_CWD_MAX - 1);
    else                          strncpy(t->cwd, "/", TASK_CWD_MAX - 1);
    t->cwd[TASK_CWD_MAX - 1] = 0;
}

task_t *task_create(const char *name, void (*entry)(void)) {
    task_t *t = (task_t *)kcalloc(sizeof(task_t));
    if (!t) return 0;
    u8 *stack = (u8 *)kmalloc(STACK_SIZE);
    if (!stack) { kfree(t); return 0; }
    paint_stack(stack);

    t->stack_base = (u64)stack;
    t->pid = next_pid++;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->state = TASK_READY;
    t->on_cpu = -1;
    fpu_blank(fpu_area_of(t));
    inherit_cwd(t);
    /* Started rather than forked, so it gets the three it is born with
       and nothing else. Inheriting would mean a program handing its
       open files to something it merely launched. */
    fd_table_init(t->fd);

    /* Build the frame an interrupt return expects to find. Long mode always
       pops rsp and ss, even returning to the same privilege level, so unlike
       32-bit there is no shorter frame for a kernel task: both fields have to
       be filled in. */
    u64 top = ((u64)stack + STACK_SIZE) & ~0xFull;
    registers_t *f = (registers_t *)(top - sizeof(registers_t));
    memset(f, 0, sizeof(*f));
    f->rip    = (u64)entry;
    f->cs     = GDT_KERNEL_CODE;
    f->ss     = GDT_KERNEL_DATA;
    f->rsp    = top;
    f->rflags = 0x202;            /* IF set: the task runs interruptible */
    f->int_no = 32;
    t->rsp = (u64)f;

    if (!head) { head = t; t->next = t; }
    else {
        task_t *p = head;
        while (p->next != head) p = p->next;
        p->next = t;
        t->next = head;
    }
    return t;
}

/* A task that is a copy of the one calling, which is what fork means.
 *
 * The address space is copied by the caller and handed in; what happens here
 * is the other half, and it is all about the frame. A task that is not
 * running has a complete interrupt frame on its kernel stack, and returning
 * to it is the interrupt return path unwinding that frame. So a forked child
 * is a frame copied from the parent's, with one register changed: the one
 * the system call's answer comes back in.
 *
 * That is the whole of the trick, and it is why both sides come back from
 * the same call with different answers.
 */
task_t *task_fork(const char *name, u64 dir, const registers_t *frame,
                  u64 child_rax) {
    task_t *parent = task_current();

    task_t *t = (task_t *)kcalloc(sizeof(task_t));
    if (!t) return 0;
    u8 *stack = (u8 *)kmalloc(STACK_SIZE);
    if (!stack) { kfree(t); return 0; }
    paint_stack(stack);

    t->stack_base = (u64)stack;
    t->pid = next_pid++;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->state = TASK_READY;
    t->on_cpu = -1;
    t->dir = dir;
    t->user = true;

    /* Everything the parent had that is not memory: where it was in the
       filesystem, how far its heap had grown, and the contents of its
       floating point registers. A child that did not inherit the heap break
       would hand out addresses the parent had already given away.

       Not the words it was started on: those are on the stack, and the
       stack is memory, so the copy of the address space already carries
       them. */
    if (parent) {
        for (u32 i = 0; i < sizeof(t->cwd); i++) t->cwd[i] = parent->cwd[i];
        t->brk = parent->brk;
        t->brk_base = parent->brk_base;
        t->parent_pid = parent->pid;
        for (u32 i = 0; i < FPU_AREA + 16; i++) t->fpu[i] = parent->fpu[i];
    } else {
        fpu_blank(fpu_area_of(t));
        strncpy(t->cwd, "/", TASK_CWD_MAX - 1);
    }

    /* The descriptors come across too, and this is not a detail: it
       is how a shell arranges a child's output. The child is handed
       the parent's table, changes one entry, and becomes a program
       that knows nothing about any of it. */
    if (parent) fd_table_clone(t->fd, parent->fd);
    else fd_table_init(t->fd);

    u64 top = ((u64)stack + STACK_SIZE) & ~0xFull;
    registers_t *f = (registers_t *)(top - sizeof(registers_t));
    *f = *frame;
    f->rax = child_rax;
    t->rsp = (u64)f;

    if (!head) { head = t; t->next = t; }
    else {
        task_t *p = head;
        while (p->next != head) p = p->next;
        p->next = t;
        t->next = head;
    }
    return t;
}

task_t *task_create_user(const char *name, u64 dir, u64 entry, u64 stack_top) {
    task_t *t = (task_t *)kcalloc(sizeof(task_t));
    if (!t) return 0;
    u8 *stack = (u8 *)kmalloc(STACK_SIZE);
    if (!stack) { kfree(t); return 0; }
    paint_stack(stack);

    t->stack_base = (u64)stack;
    t->pid = next_pid++;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->state = TASK_READY;
    t->on_cpu = -1;
    t->dir = dir;
    t->user = true;
    fpu_blank(fpu_area_of(t));
    inherit_cwd(t);
    /* Started rather than forked, so it gets the three it is born with
       and nothing else. Inheriting would mean a program handing its
       open files to something it merely launched. */
    fd_table_init(t->fd);

    /* The privilege change is what makes this frame different: the selectors
       carry a requested privilege of 3, and the stack it returns to is the
       program's rather than this one. */
    u64 top = ((u64)stack + STACK_SIZE) & ~0xFull;
    registers_t *f = (registers_t *)(top - sizeof(registers_t));
    memset(f, 0, sizeof(*f));
    f->rip    = entry;
    f->cs     = USER_CODE_SEL;
    f->ss     = USER_DATA_SEL;
    f->rsp    = stack_top;
    f->rflags = 0x202;            /* interrupts stay on in user code */
    f->int_no = 32;
    t->rsp = (u64)f;

    if (!head) { head = t; t->next = t; }
    else {
        task_t *p = head;
        while (p->next != head) p = p->next;
        p->next = t;
        t->next = head;
    }
    return t;
}

task_t *task_current(void) { return cur(); }
task_t *task_list(void)    { return head; }

task_t *task_by_pid(u32 pid) {
    if (!head) return 0;
    task_t *p = head;
    do {
        if (p->pid == pid) return p;
        p = p->next;
    } while (p != head);
    return 0;
}

bool task_alive(u32 pid) {
    task_t *t = task_by_pid(pid);
    return t && t->state != TASK_DEAD;
}

/* Blocks until the task finishes, then collects what it exited with.
 *
 * The task record is kept until somebody does this, because the status lives
 * in it. Nothing is required to: a task nobody waits for is freed when it
 * dies, and this returns -1 for it. */
int task_wait(u32 pid) {
    task_t *t = task_by_pid(pid);
    if (!t) return -1;

    /* Waiting on the record's own address, so task_exit_with can wake
       exactly the waiters for this task and nobody else. */
    while (t->state != TASK_DEAD) {
        if (!wait_on(t, 1000)) {
            /* Timed out. The task may have gone in the meantime, in which
               case the record is no longer findable. */
            if (!task_by_pid(pid)) return -1;
        }
    }

    int status = t->exit_status;
    t->reaped = true;
    return status;
}

u32 task_blocked_count(void) {
    if (!head) return 0;
    u32 n = 0;
    task_t *p = head;
    do { if (p->state == TASK_BLOCKED) n++; p = p->next; } while (p != head);
    return n;
}

u32 task_count(void) {
    if (!head) return 0;
    u32 n = 0; task_t *p = head;
    do { n++; p = p->next; } while (p != head);
    return n;
}

/* Exactly one lap of the ring, starting after whoever just ran, which is
   what makes this round robin rather than a search that favours the front
   of the list. The task that just ran is itself visited last, so a machine
   with one runnable task keeps running it.

   Two things make this a question about a processor rather than about the
   machine. A task another processor is already running is not available,
   whatever its state says -- state is about whether it wants to run and
   on_cpu is about whether it already is. And a processor's idle task is
   its own: picking somebody else's would be running one task twice.

   Kernel tasks stay on the boot processor. Everything they touch -- the
   heap, the task list, every driver -- is reached without a lock, and the
   lock that will let a processor into the kernel is about system calls
   coming in from ring 3. A kernel task migrating would be kernel code on
   two processors with nothing between them. */
static task_t *pick_next(task_t *from) {
    task_t *start = from ? from->next : head;
    if (!start) return 0;

    u32 me = smp_this_cpu();

    /* A processor with a piece of a frame waiting for it is not free. The
       compositor hands one half out and spins for the answer, so giving
       that processor a program instead turns a shared frame into a stall
       the length of the spin. Its own idle loop is where the handed work
       is picked up, so staying idle is how it gets done. */
    if (me != 0 && smp_work_pending(me) && idle_of[me]) return idle_of[me];

    u64 now = timer_ticks();
    task_t *p = start;
    task_t *idle_seen = idle_of[me];

    do {
        if (p->state == TASK_SLEEPING && now >= p->wake_at) p->state = TASK_READY;

        /* A blocked task with a deadline gets released when it passes, so a
           wakeup that never arrives is a slow operation rather than a hung
           machine. One with no deadline waits for somebody to wake it. */
        if (p->state == TASK_BLOCKED && p->wake_at && now >= p->wake_at) {
            /* The channel is deliberately left set. Clearing it is what a
               real wakeup does, so leaving it is how the waiter tells a
               deadline apart from somebody actually calling wake. */
            p->wake_at = 0;
            p->state = TASK_READY;
        }

        if (p->state == TASK_READY || p->state == TASK_RUNNING) {
            /* Idle is runnable by construction and must never be picked
               over something with work to do, so it is walked past. */
            if (is_idle(p))                          { p = p->next; continue; }
            if (p->on_cpu >= 0 && (u32)p->on_cpu != me) { p = p->next; continue; }
            if (me != 0 && !p->user)                 { p = p->next; continue; }
            return p;
        }
        p = p->next;
    } while (p && p != start);

    /* Nothing wanted the processor. Before there was an idle task the only
       answer here was `from`, and handing the processor back to a task that
       has just asked to sleep is what made a sleep mean nothing. */
    if (idle_seen) return idle_seen;
    return from;
}

/* Called from the interrupt dispatcher on every timer tick. Returns the
   stack pointer the interrupt return path should unwind. */
u64 scheduler_switch(u64 rsp) {
    if (!started || !head) return rsp;

    /* Anything raised since the last time through, acted on before anything
       is chosen to run. A task that has been interrupted should not get
       another slice first. */
    signal_take_pending();

    if (cur()) {
        cur()->rsp = rsp;

        /* The vector registers belong to whoever was running. Two tasks
           doing arithmetic at once read each other's operands without
           this, and the symptom is a wrong number rather than a crash. */
        fpu_save(fpu_area_of(cur()));

        /* Checked here because this is the one place every task passes
           through, and because the alternative is finding out from a fault
           in something unrelated. Said out loud rather than repaired: a
           stack that has already been run off the end of has written over
           whatever was underneath it, and carrying on would be carrying on
           with a heap that is no longer what it says it is. */
        if (*(const volatile u64 *)cur()->stack_base != STACK_PAINT)
            panic("the %s task ran off the end of its kernel stack", cur()->name);

        if (cur()->state == TASK_RUNNING) cur()->state = TASK_READY;

        /* Let go of it before anything else can pick it up. Held across
           the choice below, a task that this processor is putting down
           would be invisible to every other processor for the length of
           one decision -- which is not wrong, and is a processor idling
           next to work it could have had. */
        cur()->on_cpu = -1;
    }

    task_t *next = pick_next(cur());
    if (!next) return rsp;

    set_cur(next);
    cur()->on_cpu = (int)smp_this_cpu();
    cur()->state = TASK_RUNNING;
    cur()->slices++;
    if (cur()->user) sched_note_user_slice(smp_this_cpu());
    fpu_restore(fpu_area_of(cur()));

    /* The next interrupt taken in this task has to land on a stack the CPU
       can find, and in user mode it finds it here. */
    tss_set_stack(cur()->stack_base + STACK_SIZE);

    u64 want = cur()->dir ? cur()->dir : paging_kernel_directory();
    if (want != paging_current_directory()) paging_switch(want);

    /* Reap anything that finished, but never the task we are about to run.
       Switch address spaces first: the task that just exited may still own
       the active page directory, and freeing the CR3 currently in use would
       tear the floor out from under this code. */
    if (head) {
        task_t *p = head;
        for (u32 i = 0; i < 4096; i++) {
            task_t *n = p->next;
            /* A dead task is kept a little while after it finishes, so
               that whoever started it can still ask what it returned. It
               goes as soon as the status is collected, and anyway once the
               grace period is up, so a task nobody waits for is not a leak.
               Freeing it immediately is how a wait comes back with nothing. */
            bool expired = n->died_at && timer_ticks() > n->died_at + REAP_GRACE;
            bool collectable = n->reaped || expired;
            if (n != p && n->state == TASK_DEAD && collectable &&
                n != cur() && n != head) {
                p->next = n->next;
                if (n->dir) paging_free_directory(n->dir);
                kfree((void *)n->stack_base);
                kfree(n);
            } else p = n;
            if (p == head) break;
        }
    }

    return cur()->rsp;
}

void sched_adopt_ap(u32 cpu, u64 stack_base) {
    if (cpu >= SMP_MAX_CPUS || idle_of[cpu]) return;

    task_t *t = (task_t *)kcalloc(sizeof(task_t));
    if (!t) panic("no room for a task record on processor %d", cpu);

    /* The stack it is already standing on, rather than one of its own: this
       task is not started, it is recognised. The first interrupt on this
       processor saves the context it was in, and that context is here. */
    t->stack_base = stack_base;
    t->pid = next_pid++;
    strncpy(t->name, "idle", sizeof(t->name) - 1);
    t->state = TASK_RUNNING;
    t->on_cpu = (int)cpu;
    fpu_blank(fpu_area_of(t));
    strncpy(t->cwd, "/", TASK_CWD_MAX - 1);
    fd_table_init(t->fd);

    /* Into the ring, behind the head. The list is walked by the scheduler
       on the other processor, so this is done under the lock. */
    if (head) { t->next = head->next; head->next = t; }
    else      { t->next = t; head = t; }

    idle_of[cpu] = t;
    current_of[cpu] = t;
}

void sched_start(void) {
    if (!head) panic("sched_start with no tasks");

    /* Last, so that the count printed at boot is the number of tasks this
       machine was asked to run rather than that plus one.
     *
       One per processor that came up. A processor with nothing to run has
       to have somewhere of its own to be. */
    idle_of[0] = task_create("idle", idle_entry);
    if (!idle_of[0]) panic("no room for an idle task");

    /* Held from here on. This processor is in the kernel and stays there
       until the first task it picks turns out to be a program. */
    kernel_lock_acquire();

    started = true;
    sti();
    for (;;) hlt();          /* the first timer tick takes us into a task */
}

void task_yield(void) {
    /* Give up the rest of this slice. Its own vector rather than the
       timer's: what is wanted is a switch, and the timer's handler does a
       great deal besides, starting with counting a tick that has not
       happened. See VEC_YIELD in idt.h. */
    __asm__ volatile ("int %0" :: "i"(VEC_YIELD));
}

/* A switch rather than a table, because a table has to be kept the same
   length as the enum and nothing says when it is not. One was four long
   against five states: a blocked task printed as dead and a dead one read
   the pointer past the end of the array and printed the bytes of whatever
   came next. */
const char *task_state_name(task_state_t s) {
    switch (s) {
        case TASK_READY:    return "ready";
        case TASK_RUNNING:  return "running";
        case TASK_SLEEPING: return "sleeping";
        case TASK_BLOCKED:  return "blocked";
        case TASK_DEAD:     return "dead";
    }
    return "?";
}

/* Whole ticks only. A halt ended early by a key or the mouse inside the
   same tick counts as nothing, which charges the task for time it spent
   waiting. That is the direction to be wrong in: it can say busy when it
   was idle, and never idle when it was busy. */
void task_idle_wait(void) {
    if (!interrupts_enabled()) return;   /* halting here would never return */

    u64 before = timer_ticks();
    hlt();
    if (cur()) cur()->idle_ticks += timer_ticks() - before;
}

/* Rounded up, and never to nothing.
 *
 * (ms * hz) / 1000 truncates, so at a hundred hertz every sleep under ten
 * milliseconds was a sleep of no ticks at all: the task was marked sleeping
 * with a wake time already in the past and was runnable again before the
 * switch. Every caller that used a short sleep to pace something got no
 * pacing, and the one that noticed was a sound buffer that gave up after
 * two hundred and fifty two-millisecond sleeps -- a few microseconds, and
 * then a note abandoned a third of the way through.
 *
 * Asking to sleep is asking to give the processor away. A sleep too short
 * to be counted should still do that much, so anything above zero waits at
 * least one tick. */
void task_sleep(u32 ms) {
    if (!cur()) { sleep_ms(ms); return; }

    u32 hz = timer_hz() ? timer_hz() : 100;
    u64 t = ((u64)ms * hz + 999u) / 1000u;
    if (ms && !t) t = 1;

    cur()->wake_at = timer_ticks() + t;
    cur()->state = TASK_SLEEPING;
    task_yield();
}

void task_exit_with(int status) {
    /* Take back anything the task still holds. A graphical program that
       crashes must not leave its window on the desktop. */
    if (cur()) {
        winsrv_release(cur()->pid);
        fd_table_release(cur()->fd);
        syscall_release(cur()->pid);

        cur()->exit_status = status;
        cur()->died_at = timer_ticks();
        cur()->state = TASK_DEAD;

        /* Anyone waiting on this task is waiting on its record. */
        wake_all(cur());
    }
    for (;;) task_yield();
}

void task_exit(void) { task_exit_with(0); }
