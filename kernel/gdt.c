/* Segmentation in long mode, which barely exists any more.
 *
 * A 64-bit code segment has no base and no limit: the processor ignores both
 * and every address goes straight to paging. What is left in a descriptor is
 * a handful of bits saying whether it is code or data, which ring it belongs
 * to, and whether it is 64-bit. That is why the entries below set a limit of
 * zero and it does not matter.
 *
 * The task state segment survives, and for a different reason than it had in
 * 32-bit: it no longer holds a saved task, only the stack pointers the
 * processor switches to when an interrupt arrives at a higher privilege. Its
 * descriptor is sixteen bytes rather than eight, because it is the one thing
 * left that still needs a 64-bit base.
 */
#include "gdt.h"
#include "smp.h"
#include "string.h"

struct gdt_entry {
    u16 limit_low, base_low;
    u8  base_mid, access, flags, base_high;
} __attribute__((packed));

/* The system-segment form, which spills a 64-bit base across two slots. */
struct gdt_system_entry {
    u16 limit_low, base_low;
    u8  base_mid, access, flags, base_high;
    u32 base_upper;
    u32 reserved;
} __attribute__((packed));

struct gdt_ptr { u16 limit; u64 base; } __attribute__((packed));

struct tss_entry {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;      /* one stack per privilege level */
    u64 reserved1;
    u64 ist[7];                /* interrupt stack table, unused here */
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} __attribute__((packed));

/* null, kernel code, kernel data, user data, user code, and then two slots
   for every processor's task state segment. User data comes before user
   code because that is the order sysret would want, and there is no reason
   to differ from it. */
#define GDT_FIXED 5
#define GDT_SLOTS (GDT_FIXED + SMP_MAX_CPUS * 2)
static struct gdt_entry gdt[GDT_SLOTS];
static struct gdt_ptr   gdtp;

/* One per processor, each aligned, because the processor writes to the one
   it is using and two of them sharing a cache line is the kind of thing
   that is merely slow until it is not. */
static struct tss_entry tss[SMP_MAX_CPUS] __attribute__((aligned(64)));

extern void gdt_flush(u64 gdtp_addr);
extern void tss_flush(u16 selector);

static void set_gate(int i, u8 access, u8 flags) {
    gdt[i].limit_low = 0;
    gdt[i].base_low = 0;
    gdt[i].base_mid = 0;
    gdt[i].access = access;
    gdt[i].flags = flags;
    gdt[i].base_high = 0;
}

void tss_set_stack_for(u32 cpu, u64 rsp0) {
    if (cpu < SMP_MAX_CPUS) tss[cpu].rsp0 = rsp0;
}

void tss_set_stack(u64 rsp0) { tss_set_stack_for(smp_this_cpu(), rsp0); }

u64 tss_stack_of(u32 cpu) {
    return cpu < SMP_MAX_CPUS ? tss[cpu].rsp0 : 0;
}

u16 tss_current_selector(void) {
    u16 sel = 0;
    __asm__ volatile ("str %0" : "=r"(sel));
    return sel;
}

/* Fills in one processor's descriptor. The limit is the segment's own size
   and the base is where that segment is, which is the whole reason this
   descriptor is twice the width of the others. */
static void set_tss(u32 cpu) {
    u64 base = (u64)&tss[cpu];
    u32 limit = sizeof(tss[cpu]) - 1;

    memset(&tss[cpu], 0, sizeof(tss[cpu]));
    tss[cpu].iomap_base = sizeof(tss[cpu]);   /* no I/O permission bitmap */

    struct gdt_system_entry *t =
        (struct gdt_system_entry *)&gdt[GDT_TSS(cpu) / 8];
    t->limit_low  = (u16)(limit & 0xFFFF);
    t->base_low   = (u16)(base & 0xFFFF);
    t->base_mid   = (u8)((base >> 16) & 0xFF);
    t->access     = 0x89;              /* present, 64-bit available TSS */
    t->flags      = (u8)((limit >> 16) & 0x0F);
    t->base_high  = (u8)((base >> 24) & 0xFF);
    t->base_upper = (u32)(base >> 32);
    t->reserved   = 0;
}

void gdt_init(void) {
    memset(gdt, 0, sizeof(gdt));

    /* access: present | descriptor | executable | readable-writable
       flags:  the long-mode bit, which is what makes a code segment 64-bit */
    set_gate(GDT_KERNEL_CODE / 8, 0x9A, 0x20);
    set_gate(GDT_KERNEL_DATA / 8, 0x92, 0x00);
    set_gate(GDT_USER_DATA / 8,   0xF2, 0x00);
    set_gate(GDT_USER_CODE / 8,   0xFA, 0x20);

    /* Every processor's descriptor, built here on the boot one. They are
       ordinary read only data once written, so the others have nothing to
       build -- they load the table and point their task register at their
       own. */
    for (u32 i = 0; i < SMP_MAX_CPUS; i++) set_tss(i);

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (u64)&gdt;

    gdt_flush((u64)&gdtp);
    tss_flush(GDT_TSS(0));
}

void gdt_load_cpu(u32 cpu) {
    if (cpu >= SMP_MAX_CPUS) return;
    /* The same table. A processor that came up on the trampoline's own
       descriptors is still using those, and they have no TSS in them at
       all. */
    gdt_flush((u64)&gdtp);
    tss_flush(GDT_TSS(cpu));
}
