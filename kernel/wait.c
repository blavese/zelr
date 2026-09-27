/* Sleeping on something, instead of asking about it in a loop.
 *
 * A task that blocks records the address it is waiting on and stops being
 * schedulable. Whoever changes that thing wakes everyone waiting on it. The
 * scheduler never looks at a blocked task, so waiting costs nothing.
 *
 * The channel is just an address, which is what keeps this small: nothing has
 * to be declared waitable or initialised. A driver waits on its own buffer, a
 * program waits on the task it started, and neither knows about the other.
 * Two things waiting on the same address wake together, which is why the
 * caller always rechecks its own condition afterwards rather than trusting
 * that being woken means being ready.
 *
 * Everything here runs with interrupts off. A wakeup usually arrives from an
 * interrupt handler, and the window between deciding to block and actually
 * blocking is exactly where a missed wakeup would hide.
 */
#include "wait.h"
#include "sched.h"
#include "timer.h"
#include "io.h"
#include "printf.h"

static u32 wakeups;
static u32 blocked;

u32 wait_wakeups(void) { return wakeups; }
u32 wait_blocked_now(void) { return blocked; }

bool wait_on(const void *channel, u32 timeout_ms) {
    task_t *me = task_current();
    if (!me || !channel) {
        /* Before the scheduler exists there is nobody to block. Spin, which
           is what the whole kernel used to do. */
        if (timeout_ms) sleep_ms(timeout_ms);
        return false;
    }

    bool were_on = interrupts_enabled();
    cli();

    me->wait_on = channel;
    me->wake_at = timeout_ms ? timer_ticks() + (timeout_ms * timer_hz()) / 1000u + 1
                             : 0;
    me->state = TASK_BLOCKED;
    blocked++;

    if (were_on) sti();

    /* Give up the processor. The scheduler will not pick this task again
       until someone clears wait_on, or the deadline passes. */
    task_yield();

    /* Woken, one way or the other. wait_on is cleared by whoever did it. */
    bool woken = me->wait_on == 0;
    me->wait_on = 0;
    return woken;
}

/* Set when a wake makes a task runnable, and taken by the interrupt path,
   which uses it to hand an idle processor straight to that task rather than
   leave it idle until the next tick. */
static volatile bool woke;

bool wait_take_woke(void) {
    bool w = woke;
    woke = false;
    return w;
}

static void wake(const void *channel, bool only_one) {
    if (!channel) return;

    task_t *head_task = task_list();
    if (!head_task) return;

    bool were_on = interrupts_enabled();
    cli();

    task_t *p = head_task;
    do {
        if (p->state == TASK_BLOCKED && p->wait_on == channel) {
            p->wait_on = 0;
            p->wake_at = 0;
            p->state = TASK_READY;
            wakeups++;
            woke = true;
            if (blocked) blocked--;
            if (only_one) break;
        }
        p = p->next;
    } while (p != head_task);

    if (were_on) sti();
}

void wake_all(const void *channel) { wake(channel, false); }
void wake_one(const void *channel) { wake(channel, true); }
