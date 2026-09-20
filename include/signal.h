#pragma once
#include "types.h"

/* Interrupting a program.
 *
 * There is now a shell that can start one, and until this file there was no
 * way to stop one: a program that looped held the console until the machine
 * was restarted. That is not a missing convenience, it is the difference
 * between a shell you can use and a shell you can demonstrate.
 *
 * What is here is the smallest thing that is honestly a signal:
 *
 *   - a number, and a default action, which for all three of these is that
 *     the program ends
 *   - a task may say it wants one ignored, which is how a shell survives the
 *     interrupt meant for the program it started
 *   - KILL cannot be ignored, because something has to be final
 *
 * What is not here is a handler. A program cannot yet ask to be told and
 * carry on: that means building a frame on the program's own stack, pointing
 * it at a function, and arranging a way back afterwards, and it is a larger
 * thing than this. Saying so is better than a signal() that takes a function
 * and never calls it.
 *
 * Delivery happens in the scheduler, because that is the one place every
 * task passes through whatever it is doing. A program spinning in a loop
 * makes no system calls and would never notice a signal checked on the way
 * out of one — and a program spinning in a loop is exactly the program
 * somebody is trying to interrupt.
 */

#define SIGINT   2
#define SIGKILL  9
#define SIGTERM 15
#define SIG_MAX 32

/* What a task wants done with one. */
#define SIG_DFL 0
#define SIG_IGN 1

/* Marks a signal pending on a task. It is acted on the next time the
   scheduler looks at that task, which is within a tick. */
bool signal_send(u32 pid, int sig);

/* What a task wants done with a signal. KILL is refused. */
bool signal_disposition(u32 pid, int sig, int how);

/* Acts on everything pending, across every task. Called by the scheduler. */
void signal_take_pending(void);

/* --- who a keystroke is meant for -----------------------------------------
 *
 * Unix answers this with process groups: the terminal has a foreground
 * group and the interrupt goes to it. There are no groups here, so it is
 * worked out from the parent chain instead, which gives the same answer in
 * the case that matters.
 *
 * The console remembers which task last read from it — the shell, normally.
 * An interrupt goes to that task's running children if it has any, because a
 * shell waiting on a program is not the thing being interrupted; and to the
 * task itself if it has none.
 */
void signal_console_reader(u32 pid);

/* Ctrl-C, from wherever it was typed. Returns how many tasks it reached. */
int  signal_interrupt(void);

/* Ends a task, releasing everything it holds. One copy of this, because a
   task can end by exiting, by being killed and now by being signalled, and
   three copies of "everything it holds" is how one of them comes to hold
   something the other two let go. */
void signal_end_task(u32 pid, int status);
