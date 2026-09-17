#pragma once
#include "types.h"
#include "idt.h"

/* Matches VFS_PATH_MAX. Spelled out rather than included, because the
   scheduler has no other reason to know about the filesystem. */
#define TASK_CWD_MAX 128

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

#define TASK_ARG_MAX 128

typedef struct task {
    u64  rsp;                 /* saved kernel stack pointer */
    u64  stack_base;
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
    bool user;                /* runs in ring 3 */
    char cwd[TASK_CWD_MAX];   /* working directory, inherited at creation */

    /* What this program was started on, if anything: one string, which for
       everything that uses it is a path. Set before the task can run. */
    char arg[TASK_ARG_MAX];
    struct task *next;
} task_t;

void   sched_init(void);
task_t *task_create(const char *name, void (*entry)(void));

/* Builds a ring 3 task in its own address space. entry and stack_top are
   addresses in that space, not the kernel's. */
task_t *task_create_user(const char *name, u64 dir, u64 entry, u64 stack_top);
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

u32    task_count(void);
u32    task_blocked_count(void);
task_t *task_by_pid(u32 pid);
