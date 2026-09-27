#pragma once
#include "types.h"

/* Sleeping on something, instead of asking about it in a loop.
 *
 * Waiting used to be written like this:
 *
 *     while (!ready) task_yield();
 *
 * which is correct and wasteful: the task is scheduled, looks, finds nothing
 * and gives the slice back, over and over, for as long as the wait lasts. On
 * a machine doing one thing at a time nobody notices. With several tasks
 * waiting on a disk that is most of the processor spent on asking.
 *
 * A wait queue turns it round. A task that cannot proceed marks itself
 * blocked and names the thing it is waiting for; whoever changes that thing
 * wakes everyone waiting on it. A blocked task is never scheduled at all, so
 * waiting costs nothing until there is something to say.
 *
 * The queue is the address of whatever is being waited on, which means no
 * structure has to be declared or initialised to be waitable: a driver waits
 * on its own buffer, the scheduler waits on a task, and neither needs to know
 * about the other.
 */

/* Blocks until someone wakes this address, or the timeout passes. Zero means
   wait indefinitely. Returns true if it was woken, false if it timed out. */
bool wait_on(const void *channel, u32 timeout_ms);

/* Wakes everything blocked on an address. Safe from an interrupt handler,
   which is where most wakeups come from. */
void wake_all(const void *channel);

/* Wakes at most one, for the case where only one waiter can make progress. */
void wake_one(const void *channel);

/* How many wakeups have been delivered, and how many waits were satisfied
   without ever blocking. The second number is the interesting one: it says
   how often the thing being waited for was already ready. */
u32 wait_wakeups(void);

/* Whether any wake has made a task runnable since this was last asked. */
bool wait_take_woke(void);
u32 wait_blocked_now(void);
