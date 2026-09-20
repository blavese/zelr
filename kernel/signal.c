#include "signal.h"
#include "fd.h"
#include "sched.h"
#include "syscall.h"
#include "timer.h"
#include "wait.h"
#include "winsrv.h"

/* See include/signal.h for what a signal is here and what it is not. */

/* The task that last read from the console, and so the one an interrupt is
   about. Zero when nothing has read from it, which is the state a machine
   boots in. */
static u32 console_pid;

void signal_console_reader(u32 pid) { console_pid = pid; }

/* --- ending one ----------------------------------------------------------
 *
 * The same thing exiting does, done to a task that is not the one running.
 * Written here rather than a third time, because a task can now end three
 * ways and the risk is not that one of them fails: it is that one of them
 * forgets the windows, or the open files, or the socket, and nothing says
 * so until the machine has been up for an hour.
 */
void signal_end_task(u32 pid, int status) {
    task_t *t = task_by_pid(pid);
    if (!t || t->state == TASK_DEAD) return;

    winsrv_release(pid);
    fd_table_release(t->fd);
    syscall_release(pid);

    t->exit_status = status;
    t->died_at = timer_ticks();
    t->state = TASK_DEAD;

    /* Anything waiting on this task is waiting on its record. A shell
       blocked in wait is the usual one, and it has to be let go or the
       interrupt stops the program and leaves the shell holding nothing. */
    wake_all(t);
}

/* --- pending -------------------------------------------------------------- */

bool signal_send(u32 pid, int sig) {
    if (sig <= 0 || sig >= SIG_MAX) return false;
    task_t *t = task_by_pid(pid);
    if (!t || t->state == TASK_DEAD) return false;
    t->sig_pending |= 1u << sig;
    return true;
}

bool signal_disposition(u32 pid, int sig, int how) {
    if (sig <= 0 || sig >= SIG_MAX) return false;

    /* Something has to be final. A program that could refuse this could not
       be stopped by anything short of the power. */
    if (sig == SIGKILL) return false;

    task_t *t = task_by_pid(pid);
    if (!t) return false;

    if (how == SIG_IGN) t->sig_ignored |= 1u << sig;
    else t->sig_ignored &= ~(1u << sig);
    return true;
}

/* Everything pending, acted on. The default action for all three of these
   is that the program ends, so this is one loop and not a table. */
void signal_take_pending(void) {
    task_t *head = task_list();
    if (!head) return;

    task_t *t = head;
    for (u32 guard = 0; guard < 4096; guard++) {
        if (t->state != TASK_DEAD && t->sig_pending) {
            u32 live = t->sig_pending & ~t->sig_ignored;
            t->sig_pending = 0;            /* ignored ones are not kept */

            if (live) {
                /* The lowest one, and the status a shell would report:
                   128 plus the number, which is what every shell has said
                   since the seventies. */
                int sig = 0;
                for (int i = 1; i < SIG_MAX; i++)
                    if (live & (1u << i)) { sig = i; break; }
                if (sig) signal_end_task(t->pid, 128 + sig);
            }
        }
        t = t->next;
        if (!t || t == head) break;
    }
}

/* --- what ctrl-C means ---------------------------------------------------- */

int signal_interrupt(void) {
    if (!console_pid) return 0;

    task_t *head = task_list();
    if (!head) return 0;

    /* The console reader's running children first. A shell that started a
       program and is waiting for it is not the thing being interrupted. */
    int sent = 0;
    task_t *t = head;
    for (u32 guard = 0; guard < 4096; guard++) {
        if (t->state != TASK_DEAD && t->parent_pid == console_pid &&
            t->pid != console_pid) {
            t->sig_pending |= 1u << SIGINT;
            sent++;
        }
        t = t->next;
        if (!t || t == head) break;
    }
    if (sent) return sent;

    /* Nothing running under it, so it is the one that meant to be
       interrupted. A shell sitting at its prompt ignores this, which is why
       ctrl-C there does nothing rather than ending the session. */
    return signal_send(console_pid, SIGINT) ? 1 : 0;
}
