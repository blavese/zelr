/* Round-robin preemptive scheduler.
 *
 * Every task owns a kernel stack. A task that is not running has a complete
 * interrupt frame sitting on that stack, so switching tasks is just a matter
 * of telling the interrupt return path to unwind a different one. */
#include "sched.h"
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

static void paint_stack(u8 *stack) {
    u64 *w = (u64 *)stack;
    for (u32 i = 0; i < STACK_SIZE / 8; i++) w[i] = STACK_PAINT;
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
static task_t *current;
static u32 next_pid = 1;
static bool started = false;

void sched_init(void) { head = current = 0; next_pid = 1; started = false; }

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
    inherit_cwd(t);

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
    t->dir = dir;
    t->user = true;
    inherit_cwd(t);

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

task_t *task_current(void) { return current; }
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

static task_t *pick_next(task_t *from) {
    task_t *p = from ? from->next : head;
    u64 now = timer_ticks();
    for (u32 i = 0; i < 4096 && p; i++, p = p->next) {
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

        if (p->state == TASK_READY || p->state == TASK_RUNNING) return p;
    }
    return from;
}

/* Called from the interrupt dispatcher on every timer tick. Returns the
   stack pointer the interrupt return path should unwind. */
u64 scheduler_switch(u64 rsp) {
    if (!started || !head) return rsp;

    if (current) {
        current->rsp = rsp;

        /* Checked here because this is the one place every task passes
           through, and because the alternative is finding out from a fault
           in something unrelated. Said out loud rather than repaired: a
           stack that has already been run off the end of has written over
           whatever was underneath it, and carrying on would be carrying on
           with a heap that is no longer what it says it is. */
        if (*(const volatile u64 *)current->stack_base != STACK_PAINT)
            panic("the %s task ran off the end of its kernel stack", current->name);

        if (current->state == TASK_RUNNING) current->state = TASK_READY;
    }

    task_t *next = pick_next(current);
    if (!next) return rsp;

    current = next;
    current->state = TASK_RUNNING;
    current->slices++;

    /* The next interrupt taken in this task has to land on a stack the CPU
       can find, and in user mode it finds it here. */
    tss_set_stack(current->stack_base + STACK_SIZE);

    u64 want = current->dir ? current->dir : paging_kernel_directory();
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
                n != current && n != head) {
                p->next = n->next;
                if (n->dir) paging_free_directory(n->dir);
                kfree((void *)n->stack_base);
                kfree(n);
            } else p = n;
            if (p == head) break;
        }
    }

    return current->rsp;
}

void sched_start(void) {
    if (!head) panic("sched_start with no tasks");
    started = true;
    sti();
    for (;;) hlt();          /* the first timer tick takes us into a task */
}

void task_yield(void) {
    /* Give up the rest of this slice by asking for a tick early. */
    __asm__ volatile ("int $32");
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
    if (current) current->idle_ticks += timer_ticks() - before;
}

void task_sleep(u32 ms) {
    if (!current) { sleep_ms(ms); return; }
    current->wake_at = timer_ticks() + (ms * timer_hz()) / 1000u;
    current->state = TASK_SLEEPING;
    task_yield();
}

void task_exit_with(int status) {
    /* Take back anything the task still holds. A graphical program that
       crashes must not leave its window on the desktop. */
    if (current) {
        winsrv_release(current->pid);
        vfs_release(current->pid);
        syscall_release(current->pid);

        current->exit_status = status;
        current->died_at = timer_ticks();
        current->state = TASK_DEAD;

        /* Anyone waiting on this task is waiting on its record. */
        wake_all(current);
    }
    for (;;) task_yield();
}

void task_exit(void) { task_exit_with(0); }
