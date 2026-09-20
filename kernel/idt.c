#include "idt.h"
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

/* Called from isr_common in isr.S */
u64 isr_dispatch(registers_t *r) {
    if (handlers[r->int_no]) handlers[r->int_no](r);
    else if (r->int_no < 32) {
        u64 cr2 = 0;
        if (r->int_no == 14) __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
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
    } else if (r->int_no == VEC_AP_WAKE) {
        /* Nothing outside the processor delivered this, so only its own
           local APIC has to be told. Forgetting leaves the in-service bit
           set and nothing at that priority is ever delivered again, which
           is a processor that wakes exactly once. */
        lapic_eoi();
    }

    /* The scheduler may hand back a different task's frame. */
    u64 resume = (u64)r;
    if (r->int_no == 32 || r->int_no == VEC_YIELD)
        resume = scheduler_switch(resume);
    return resume;
}
