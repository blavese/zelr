#include "idt.h"
#include "sched.h"
#include "smp.h"
#include "paging.h"
#include "user.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pic.h"
#include "gdt.h"
#include "blackbox.h"
#include "ioapic.h"
#include "lapic.h"

u64 scheduler_switch(u64 rsp);

/* A gate is sixteen bytes in long mode rather than eight: the handler address
   is 64-bit, and there is a field for the interrupt stack table, which this
   kernel does not use. */
struct idt_entry {
    u16 base_low;
    u16 sel;
    u8  ist;
    u8  flags;
    u16 base_mid;
    u32 base_high;
    u32 reserved;
} __attribute__((packed));

struct idt_ptr { u16 limit; u64 base; } __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;
static isr_handler_t    handlers[256];

extern void idt_flush(u64);
extern void *isr_stub_table[];

static void set_gate(u8 n, u64 base, u16 sel, u8 flags) {
    idt[n].base_low  = (u16)(base & 0xFFFF);
    idt[n].base_mid  = (u16)((base >> 16) & 0xFFFF);
    idt[n].base_high = (u32)(base >> 32);
    idt[n].sel = sel;
    idt[n].ist = 0;
    idt[n].flags = flags;
    idt[n].reserved = 0;
}

void register_interrupt_handler(u8 n, isr_handler_t h) { handlers[n] = h; }

bool idt_has_handler(u8 n) { return handlers[n] != 0; }

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (u64)&idt;
    memset(&idt, 0, sizeof(idt));
    memset(&handlers, 0, sizeof(handlers));

    for (int i = 0; i < 48; i++)
        set_gate((u8)i, (u64)isr_stub_table[i], GDT_KERNEL_CODE, 0x8E);

    /* 0x80 is the syscall gate: DPL 3 so ring 3 may invoke it. */
    set_gate(0x80, (u64)isr_stub_table[48], GDT_KERNEL_CODE, 0xEE);

    /* And the wake-up one processor sends another. Ring 0 only. */
    set_gate(VEC_AP_WAKE, (u64)isr_stub_table[49], GDT_KERNEL_CODE, 0x8E);

    /* And the one a task raises on itself to be switched away from. Ring 0:
       a program asks through the system call gate, not through this. */
    set_gate(VEC_YIELD, (u64)isr_stub_table[50], GDT_KERNEL_CODE, 0x8E);

    /* Every processor's own timer, delivered through its own local APIC. */
    set_gate(VEC_LOCAL_TIMER, (u64)isr_stub_table[51], GDT_KERNEL_CODE, 0x8E);

    idt_flush((u64)&idtp);
}

void idt_load(void) { idt_flush((u64)&idtp); }

static const char *EXC[] = {
    "divide by zero", "debug", "non-maskable interrupt", "breakpoint",
    "overflow", "bound range exceeded", "invalid opcode", "device not available",
    "double fault", "coprocessor segment overrun", "invalid TSS", "segment not present",
    "stack-segment fault", "general protection fault", "page fault", "reserved",
    "x87 floating point", "alignment check", "machine check", "SIMD floating point",
    "virtualization", "control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
    "hypervisor injection", "VMM communication", "security", "reserved"
};

/* Whether a frame belongs to a program rather than to the kernel. The low
   two bits of the saved code selector are the privilege it was running at,
   and three is ring 3. */
static inline bool from_user(const registers_t *r) { return (r->cs & 3) == 3; }

/* A program that did something it cannot do ends. The machine does not.
 *
 * Until this, every exception went to panic, including one raised by a ring
 * 3 program running off the end of its own memory. A program with a bad
 * pointer took the whole machine with it -- the desktop, the other
 * programs, the disk half written -- and the account of it was a panic
 * screen naming the program, which is the right information attached to
 * altogether the wrong outcome.
 *
 * The kernel's own faults still panic, and should. A program faulting means
 * the program is wrong; the kernel faulting means the kernel's idea of its
 * own memory is wrong, and carrying on from there writes that wrongness to
 * a disk.
 *
 * task_exit_with does not return -- it ends in a yield that never comes
 * back -- so this is the last thing the faulting task does, in the same way
 * sys_exit is the last thing an exiting one does, and by the same path.
 */
static bool end_the_program(registers_t *r, u64 addr) {
    if (!from_user(r)) return false;

    task_t *t = task_current();
    const char *name = t ? t->name : "a program";
    const char *what = r->int_no < 32 ? EXC[r->int_no] : "fault";

    /* One line, into the black box, which is also the serial log: on real
       hardware that is the only account of a fault that survives it.

       One line and not the register dump bb_fault writes. That dump is
       for a fault that takes the machine down, where it is the last
       thing anybody gets; this is an ordinary event that may happen a
       hundred times in a row, and a hundred register dumps is a wall of
       text with the one useful line buried in it. Measured: a hundred
       faulting programs printed five hundred lines. */
    if (r->int_no == 14)
        bb_log("%s ended: %s at %p, rip %p [%s %s]", name, what,
               (void *)addr, (void *)r->rip,
               (r->err_code & 1) ? "protection" : "not-present",
               (r->err_code & 2) ? "write" : "read");
    else
        bb_log("%s ended: %s, rip %p", name, what, (void *)r->rip);

    /* 139 is what a shell prints for a program killed by a memory fault,
       everywhere, and has since the seventies. There is no SIGSEGV to catch
       here -- the default action is the whole of it -- so the number is the
       convention rather than a signal that was delivered. */
    task_exit_with(139);
    return true;                       /* not reached */
}

/* Called from isr_common in isr.S */
u64 isr_dispatch(registers_t *r) {
    /* The door into the kernel.
     *
     * A processor holds the kernel lock whenever it is not running ring 3
     * code, so arriving from a program is where it is taken and returning
     * to one is where it is given back. Arriving from the kernel means this
     * processor already has it and must not ask again -- there is no path
     * that takes it twice, which is what lets it be a plain spinlock.
     *
     * Interrupts are off for all of this: every gate in this table is an
     * interrupt gate, including the system call one, so nothing lands in
     * the middle of a processor holding this. */
    /* The signal that wakes a halted processor is answered before any of
       that. It touches nothing the lock covers -- its whole purpose is to
       end a halt -- and queueing for the lock here is a processor that has
       stopped answering the one holding it. */
    if (r->int_no == VEC_LOCAL_TIMER) smp_note_tick(smp_this_cpu());
    if (r->int_no == VEC_AP_WAKE) { lapic_eoi(); return (u64)r; }

    if (!kernel_lock_held_here()) {
        /* A processor's own timer does not wait for it.
         *
         * Preemption is worth having and is not worth blocking for: the
         * only thing this interrupt wants the lock for is to look through
         * the task ring, and the ring will still be there a hundredth of a
         * second later. Waiting here is worse than skipping, because the
         * wait happens with interrupts off on a processor whose idle loop
         * is also where work handed to it gets picked up -- so a processor
         * queueing for the lock is a processor that has stopped answering
         * the one holding it. That was not a theory: it deadlocked the
         * compositor's half-a-frame handoff the first time this ran on
         * four processors. */
        if (r->int_no == VEC_LOCAL_TIMER) {
            if (!kernel_lock_try()) {
                smp_note_lock_miss(smp_this_cpu());
                lapic_eoi();
                return (u64)r;
            }
        } else {
            kernel_lock_acquire();
        }
    }
    /* A write to a page that is present is the shape of a copy on write
     * fault and of nothing else here, so it is tried before anything else
     * is done about the fault.
     *
     * The user bit in the error code is deliberately not required. A system
     * call that writes its answer into the caller's memory does that write
     * from ring 0, and the page it lands on is as likely to be shared as
     * any other; requiring the bit would turn the first such call after a
     * fork into an unhandled exception in the kernel.
     */
    if (r->int_no == 14 && (r->err_code & 0x3) == 0x3) {
        u64 cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        if (paging_resolve_cow(paging_current_directory(), cr2))
            return (u64)r;
    }

    if (handlers[r->int_no]) handlers[r->int_no](r);
    else if (r->int_no < 32) {
        u64 cr2 = 0;
        if (r->int_no == 14) __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        /* A page inside something the program asked to have mapped, handed
         * over now that it has been reached for.
         *
           This is what makes a mapping cost what it is used rather than
           what it asked for, and it has to be tried before the program is
           blamed: from here, a program touching memory it was promised and
           a program touching memory that was never its look identical, and
           the list of what it asked for is the only thing that tells them
           apart. */
        if (r->int_no == 14 && from_user(r) && user_fault_fill(cr2, r->err_code))
            return (u64)r;

        /* A program's own fault is the program's problem. Everything from
           a divide by zero to a bad jump arrives here, and none of it is a
           reason to stop the machine. */
        if (end_the_program(r, cr2)) return (u64)r;   /* does not return */

        /* Into the black box before anything else touches the machine: on
           real hardware this is the only account of the fault that survives
           it, and panic below never returns. */
        bb_fault(r, EXC[r->int_no]);
        panic("unhandled exception %d (%s)\n"
              "  rip=%p err=%x cs=%x rflags=%x\n"
              "  rsp=%p cr2=%p",
              (u32)r->int_no, EXC[r->int_no], (void *)r->rip, (u32)r->err_code,
              (u32)r->cs, (u32)r->rflags, (void *)r->rsp, (void *)cr2);
    }

    /* A hardware interrupt has to be acknowledged, or nothing at the same or
       lower priority is ever delivered again. Which controller to tell
       depends on which one delivered it, and once the IOAPIC is routing,
       the 8259 is masked and no longer has anything to acknowledge. */
    if (r->int_no >= 32 && r->int_no < 48) {
        if (ioapic_active()) lapic_eoi();
        else                 pic_eoi((u8)(r->int_no - 32));
    } else if (r->int_no == VEC_LOCAL_TIMER || r->int_no == VEC_AP_WAKE) {
        /* Nothing outside the processor delivered this, so only its own
           local APIC has to be told. Forgetting leaves the in-service bit
           set and nothing at that priority is ever delivered again, which
           is a processor that wakes exactly once. */
        lapic_eoi();
    }

    /* The scheduler may hand back a different task's frame. */
    u64 resume = (u64)r;
    if (r->int_no == 32 || r->int_no == VEC_YIELD ||
        r->int_no == VEC_LOCAL_TIMER)
        resume = scheduler_switch(resume);

    /* And out of it. What decides is the frame this processor is about to
       return through, not the one it arrived on: after a switch they belong
       to different tasks.
     *
       Ring 3 is the only place that does not need the lock. Asking instead
       whether the task is a program was wrong in a way that took three runs
       in ten to show: a program preempted in the middle of a system call is
       a program by that test, so it was resumed without the lock and went
       on executing kernel code with nothing holding anybody else out. What
       the saved frame says is where the task actually is, which is the
       question.
     *
       The exception is a processor going to sleep. An idle task's frame is
       in ring 0 like any other kernel task's, and a sleeping processor
       holding the one lock is how it would stop every other one from making
       a system call. */
    registers_t *back = (registers_t *)resume;

    /* A signal with a handler is delivered here and nowhere else.
     *
       It means writing to the task's own stack, which is only mapped while
       that task's address space is the current one -- which it is, exactly
       here, and is not in the scheduler where the default action is taken.
       After the switch above, so it is the task actually going back to ring
       3 rather than the one this processor arrived on. */
    if (from_user(back)) signal_deliver(back);

    if (from_user(back) || task_is_idle(task_current())) kernel_lock_release();
    return resume;
}
