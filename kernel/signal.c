#include "signal.h"
#include "fd.h"
#include "gdt.h"
#include "paging.h"
#include "pmm.h"
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

bool signal_disposition(u32 pid, int sig, u64 how, u64 trampoline) {
    if (sig <= 0 || sig >= SIG_MAX) return false;

    /* Something has to be final. A program that could refuse this could not
       be stopped by anything short of the power, and a program that could
       catch it could decline to die. */
    if (sig == SIGKILL) return false;

    task_t *t = task_by_pid(pid);
    if (!t) return false;

    if (how != SIG_DFL && how != SIG_IGN) {
        /* A handler, so there has to be somewhere in ring 3 for it to
           return to and both addresses have to be in ring 3's half. A
           kernel address here would be a program asking to have the kernel
           jump into the kernel with a frame the program laid out. */
        if (!trampoline) return false;
        if (how < USER_SPACE_BASE || trampoline < USER_SPACE_BASE) return false;
        t->sig_trampoline = trampoline;
    }

    t->sig_handler[sig] = how;
    return true;
}

void signal_forget_handlers(u32 pid) {
    task_t *t = task_by_pid(pid);
    if (!t) return;
    for (int i = 0; i < SIG_MAX; i++)
        if (t->sig_handler[i] > SIG_IGN) t->sig_handler[i] = SIG_DFL;
    t->sig_trampoline = 0;
    t->sig_running = 0;
}

/* Everything pending, acted on. The default action for all three of these
   is that the program ends, so this is one loop and not a table. */
void signal_take_pending(void) {
    task_t *head = task_list();
    if (!head) return;

    task_t *t = head;
    for (u32 guard = 0; guard < 4096; guard++) {
        if (t->state != TASK_DEAD && t->sig_pending) {
            /* Only the ones this task has nothing else in mind for.
               Ignored signals are dropped; a signal with a handler is left
               pending, because acting on it here would end a program that
               asked to be told about it -- and this runs over tasks that
               are not the one running, whose memory is not mapped, so the
               frame a handler needs cannot be built from here. */
            u32 live = 0;
            for (int i = 1; i < SIG_MAX; i++) {
                if (!(t->sig_pending & (1u << i))) continue;
                if (t->sig_handler[i] == SIG_IGN) continue;
                if (t->sig_handler[i] != SIG_DFL) continue;   /* left for later */
                live |= 1u << i;
            }
            /* What is left pending is exactly what has a handler. */
            for (int i = 1; i < SIG_MAX; i++)
                if (t->sig_handler[i] <= SIG_IGN) t->sig_pending &= ~(1u << i);

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

/* --- a handler, which runs in ring 3 -------------------------------------
 *
 * Delivering one means interrupting a program between two of its own
 * instructions and persuading it to call a function it never called, and
 * then to carry on afterwards as though nothing had happened. The whole of
 * that is arithmetic on two things: the frame the processor is about to
 * return through, and the program's own stack.
 *
 * What goes on the stack, from the interrupted stack pointer downward:
 *
 *     the frame the program was interrupted at, all of it
 *     the address of the trampoline        <- the new stack pointer
 *
 * and the frame is then rewritten to enter the handler with the number in
 * rdi. The handler is an ordinary compiled function: it returns with a ret,
 * which pops the trampoline address, which asks for the frame back.
 *
 * The saved frame is on the program's own stack and the program can write
 * over it. That is not a hole -- it is the program's own memory and its own
 * execution -- but it does mean signal_return has to check what it finds
 * there rather than believe it.
 */

/* Whether a range of the program's stack is really there and really the
   program's. Asked before anything is written, because the alternative is a
   page fault in ring 0 inside the return path, which is not a fault the
   kernel can report against the program that caused it.

   A program that has run its stack down to the last page is the case this
   is about, and the answer for it is that the signal cannot be delivered. */
static bool stack_is_there(u64 lo, u64 hi) {
    u64 dir = paging_current_directory();
    for (u64 a = lo & ~0xFFFull; a < hi; a += PAGE_SIZE)
        if (!virt_is_user_in(dir, a)) return false;
    return true;
}

void signal_deliver(registers_t *r) {
    task_t *t = task_current();
    if (!t || t->state == TASK_DEAD || !t->sig_pending) return;

    for (int sig = 1; sig < SIG_MAX; sig++) {
        if (!(t->sig_pending & (1u << sig))) continue;

        u64 h = t->sig_handler[sig];
        if (h <= SIG_IGN) continue;            /* the scheduler's business */

        /* Already inside its own handler. Left pending rather than dropped,
           so it arrives when that one returns: a handler entered on top of
           itself is how a program held down by repeated interrupts runs out
           of stack instead of doing its work. */
        if (t->sig_running & (1u << sig)) continue;

        /* Room for the frame and the address under it, on a sixteen byte
           boundary, which is what the handler is compiled expecting. The
           red zone is not stepped over because these programs are built
           with -mno-red-zone; sdk/README.md says why that flag is there and
           this is the reason. */
        u64 sp = (r->rsp - sizeof(registers_t)) & ~15ull;
        if (sp < sizeof(registers_t) + 16) continue;      /* would wrap */
        if (!stack_is_there(sp - 16, r->rsp)) {
            /* No stack to build it on. A program that cannot be told is a
               program that gets the default, which is that it ends -- and
               saying so beats silently dropping the signal. */
            t->sig_pending &= ~(1u << sig);
            signal_end_task(t->pid, 128 + sig);
            return;
        }

        registers_t *saved = (registers_t *)sp;
        *saved = *r;

        u64 back = sp - 8;
        *(u64 *)back = t->sig_trampoline;

        t->sig_pending &= ~(1u << sig);
        t->sig_running |= 1u << sig;

        r->rsp = back;
        r->rip = h;
        r->rdi = (u64)sig;         /* where a compiled function looks */
        r->rax = 0;

        /* One per return to ring 3. The handler runs as ordinary code and
           can be interrupted itself, so anything else still pending arrives
           the next time this processor goes back. */
        return;
    }
}

bool signal_return(registers_t *r) {
    task_t *t = task_current();
    if (!t) return false;

    /* The handler has returned, which popped the trampoline address, so the
       stack pointer is back at the frame. */
    u64 at = r->rsp;
    if (at & 15) return false;
    if (!stack_is_there(at, at + sizeof(registers_t))) return false;

    const registers_t *saved = (const registers_t *)at;

    /* Checked rather than believed. This came off the program's own stack
       and the program could have written anything there; what it must not
       be able to write is a frame that returns somewhere other than ring 3
       with flags of its choosing. */
    if ((saved->cs & 3) != 3 || (saved->ss & 3) != 3) return false;
    if (saved->rip < USER_SPACE_BASE) return false;

    u64 int_no = r->int_no, err = r->err_code;
    *r = *saved;
    r->int_no = int_no;
    r->err_code = err;

    /* The selectors are the kernel's answer and not the program's, and the
       flags are masked to the ones it is allowed to choose. Interrupts go
       back on whatever it asked for: a program returning with them off
       would be a program that had stopped the machine. */
    r->cs = USER_CODE_SEL;
    r->ss = USER_DATA_SEL;
    r->rflags = (saved->rflags & 0x0CD5ull) | 0x202ull;

    /* And the signal can arrive again. */
    for (int sig = 1; sig < SIG_MAX; sig++)
        if (t->sig_running & (1u << sig)) { t->sig_running &= ~(1u << sig); break; }
    return true;
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
