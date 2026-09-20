#pragma once
#include "fpu.h"
#include "types.h"
#include "idt.h"

/* Matches VFS_PATH_MAX. Spelled out rather than included, because the
   scheduler has no other reason to know about the filesystem. */
#define TASK_CWD_MAX 128

/* Every task gets a kernel stack of this size, and nothing grows one, so it
   has to be big enough for the deepest path in the kernel with an interrupt
   landing at the bottom of it.
 *
 * That path is certificate verification, and it is genuinely large: a
 * parsed certificate carries a 4096 bit modulus, and the modular
 * exponentiation underneath it holds five numbers of that size at once plus
 * a product one limb wider. It comes to a little over sixteen kilobytes,
 * which is what 16384 used to be, and the margin was 168 bytes. The timer
 * interrupt spends a few hundred of those the moment it lands in the middle
 * of the arithmetic, and then it is the heap block underneath the stack
 * being written to rather than the stack. */
#define TASK_STACK_SIZE 32768u

/* BLOCKED is different from SLEEPING: a sleeping task has a time it wants to
   wake at, a blocked one is waiting for something to happen and may have no
   deadline at all. The scheduler skips both, but only one of them can be
   woken early by another task. */
typedef enum {
    TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_BLOCKED, TASK_DEAD
} task_state_t;

/* The name of a state, for anything in the kernel that prints one. Written
   once because it was written twice: a table of four indexed by a state that
   has five in it read past the end of itself, and `ps` printed the bytes of
   whatever came after it in place of the word. */
const char *task_state_name(task_state_t s);

/* How many files one task can have open. Matches FD_MAX in include/fd.h,
   spelled out here for the same reason TASK_CWD_MAX is: the scheduler holds
   the array and has no other reason to know what is in it. kernel/fd.c
   asserts the two numbers are still the same. */
#define TASK_MAX_FD 16

#define TASK_ARG_MAX 128

typedef struct task {
    u64  rsp;                 /* saved kernel stack pointer */
    u64  stack_base;

    /* The floating point and vector registers, as FXSAVE writes them.
     *
       Sixteen bytes more than the instruction needs, because FXSAVE faults
       unless the address is 16 byte aligned and the heap this task record
       came from only promises 8. The extra is the slack the alignment is
       taken out of, which costs sixteen bytes per task and removes a whole
       class of fault that would otherwise depend on where the allocator
       happened to put things. */
    u8   fpu[FPU_AREA + 16];
    u32  pid;
    char name[32];
    task_state_t state;
    u64  wake_at;             /* tick to wake on; 0 means no deadline */
    const void *wait_on;      /* what it is blocked on, or null */
    int  exit_status;         /* what it returned, once it is dead */
    bool reaped;              /* somebody has collected that status */
    u64  died_at;             /* when it finished, for the grace period */
    u32  slices;              /* how many times it has been scheduled */

    /* Of those, the ones it spent halted waiting for something to happen.
       The scheduler cannot tell work from waiting: it hands out a slice on
       every tick to whatever is runnable, and a task sitting in a poll loop
       looks exactly like a task doing arithmetic. So the waiting says so
       itself, and slices minus these is what the task actually did. */
    u64  idle_ticks;
    u64  dir;                 /* address space, 0 means the kernel's */

    /* Where this program's heap has grown to.
     *
       Nothing in ring 3 could allocate at all before this: every program
       was a set of fixed arrays decided at compile time, and the browser's
       limit on how big a page it could show was a number in a header. A
       program asks for more by moving this, and the pages behind it are
       mapped as it moves.

       Zero until the first request, because a program that never allocates
       should not be charged a page for the privilege. */
    u64  brk, brk_base;

    /* Who forked this one, so a program can be asked about its parent and so
       a wait can be refused when the waiter is not entitled to it. Zero for
       anything the kernel started itself. */
    u32  parent_pid;
    bool user;                /* runs in ring 3 */
    char cwd[TASK_CWD_MAX];   /* working directory, inherited at creation */

    /* What this program was started on, if anything: one string, which for
       everything that uses it is a path. Set before the task can run. */
    char arg[TASK_ARG_MAX];

    /* The numbers this task uses for its open files, each an index into the
       machine's table of them, or -1. Per task rather than global, which is
       what makes 1 mean this program's output rather than whatever was
       opened ninth on this machine. Copied by fork and kept by exec. */
    i16  fd[TASK_MAX_FD];

    /* What has been raised against this task and what it wants ignored,
       a bit per signal. Acted on by the scheduler rather than at the end
       of a system call, because a program spinning in a loop makes no
       system calls and is exactly the program somebody is interrupting. */
    u32  sig_pending;
    u32  sig_ignored;
    struct task *next;
} task_t;

void   sched_init(void);
task_t *task_create(const char *name, void (*entry)(void));

/* Builds a ring 3 task in its own address space. entry and stack_top are
   addresses in that space, not the kernel's. */
task_t *task_create_user(const char *name, u64 dir, u64 entry, u64 stack_top);

/* A copy of the calling task, sharing nothing but the code it was built
   from. `frame` is the interrupt frame the caller will itself return
   through, and `child_rax` is what the copy finds in the register the
   system call's answer arrives in. */
task_t *task_fork(const char *name, u64 dir, const registers_t *frame,
                  u64 child_rax);
void   sched_start(void);

/* Ends the running task with a status somebody may later collect. */
void   task_exit_with(int status);
void   task_exit(void);
void   task_sleep(u32 ms);

/* Parks the processor until something happens, and does not count the time
   against the task. For a loop that has nothing to do until input arrives or
   the next frame is due: it is a halt, so any interrupt ends it, and the
   ticks it slept through are taken off what the task is charged for. */
void   task_idle_wait(void);
void   task_yield(void);
task_t *task_current(void);
task_t *task_list(void);

/* True while a pid is still in the run queue and not finished. */
bool   task_alive(u32 pid);

/* Blocks until the task finishes and returns what it exited with, or -1 if
   there is no such task. Collecting the status is what lets the task record
   finally be freed. */
int    task_wait(u32 pid);

/* Bytes of this task's kernel stack that nothing has written yet. Kernel
   stacks are painted at creation, so this is a high water mark and not a
   reading of where the stack happens to be now. Zero would mean the whole
   of it has been used, which is one step from using more than there is. */
u32    task_stack_headroom(const task_t *t);

u32    task_count(void);
u32    task_blocked_count(void);
task_t *task_by_pid(u32 pid);
