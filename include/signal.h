#pragma once
#include "types.h"
#include "idt.h"

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
 * A program may also ask to be told and carry on, which is what a handler
 * is. That needs three things the other two dispositions do not:
 *
 *   - a frame built on the program's own stack, holding everything the
 *     interrupted code was using, because the handler is about to use the
 *     registers it was using
 *   - the program pointed at its function with the number in the first
 *     argument, which is where a compiled function looks for one
 *   - a way back. The handler is an ordinary function and ends in a ret, so
 *     something has to be under it to return to, and that something has to
 *     be in ring 3. The kernel has no code mapped there, so the program
 *     supplies the address when it asks for the handler, and sdk/zelr.h is
 *     where the few instructions live.
 *
 * SIGKILL cannot be caught, for the same reason it cannot be ignored.
 *
 * Delivery of the default action happens in the scheduler, because that is
 * the one place every task passes through whatever it is doing: a program
 * spinning in a loop makes no system calls and would never notice a signal
 * checked on the way out of one, and a program spinning in a loop is exactly
 * the program somebody is trying to interrupt.
 *
 * A handler is delivered somewhere else -- on the way back to ring 3, by
 * whichever processor is about to go there. It has to be, because building
 * that frame means writing to the task's own stack in the task's own address
 * space, and the scheduler walks tasks that are not running and whose memory
 * is not mapped.
 */

#define SIGINT   2
#define SIGKILL  9
#define SIGTERM 15
#define SIG_MAX 32

/* What a task wants done with one. Anything above these two is the address
   of a function in ring 3. */
#define SIG_DFL 0
#define SIG_IGN 1

/* Marks a signal pending on a task. It is acted on the next time the
   scheduler looks at that task, which is within a tick. */
bool signal_send(u32 pid, int sig);

/* What a task wants done with a signal: SIG_DFL, SIG_IGN, or the address of
   a handler, in which case `trampoline` is where that handler returns to.
   KILL is refused. */
bool signal_disposition(u32 pid, int sig, u64 how, u64 trampoline);

/* Forgets every handler, which is what exec does: the addresses belonged to
   the program that is being replaced. */
void signal_forget_handlers(u32 pid);

/* Delivers one pending signal that has a handler, by rewriting the frame the
   processor is about to return through and building the rest on the task's
   own stack. Called on the way out to ring 3 and nowhere else.

   One per return, lowest number first. A handler is a function that runs in
   ring 3 and can be interrupted itself, so there is no need to drain the
   queue here; what is left stays pending and arrives next time. */
void signal_deliver(registers_t *r);

/* Puts back what signal_deliver saved, and lets that signal be delivered
   again. The frame comes off the program's own stack, so it is checked
   rather than trusted: a program can write anything there, and what it must
   not be able to write is a frame that returns to ring 0. */
bool signal_return(registers_t *r);

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
