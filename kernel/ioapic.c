/* See include/ioapic.h.
 *
 * Registers are reached through a window rather than directly: write the
 * register number to one address, read or write its value at another. That
 * makes every access two writes and means nothing here can be done with a
 * single instruction, which matters only in that the two halves of a 64-bit
 * redirection entry are written separately, and between those two writes the
 * entry is half of one thing and half of another. So an entry is always
 * masked before it is changed. */
#include "ioapic.h"
#include "lapic.h"
#include "acpi.h"
#include "paging.h"
#include "printf.h"
#include "string.h"

#define IOREGSEL 0x00
#define IOWIN    0x10

#define REG_ID      0x00
#define REG_VER     0x01
#define REG_REDIR   0x10          /* two registers per input, from here */

/* The low half of a redirection entry. */
#define DELIVERY_FIXED   (0u << 8)
#define DEST_PHYSICAL    (0u << 11)
#define POLARITY_LOW     (1u << 13)
#define TRIGGER_LEVEL    (1u << 15)
#define ENTRY_MASKED     (1u << 16)

typedef struct {
    volatile u8 *regs;
    u32 gsi_base;
    u32 inputs;
} chip_t;

static chip_t chips[ACPI_MAX_IOAPIC];
static u32    nchips;
static bool   ready;

bool ioapic_active(void) { return ready; }

u32 ioapic_inputs(void) {
    u32 n = 0;
    for (u32 i = 0; i < nchips; i++) n += chips[i].inputs;
    return n;
}

static u32 reg_read(chip_t *c, u32 reg) {
    *(volatile u32 *)(c->regs + IOREGSEL) = reg;
    return *(volatile u32 *)(c->regs + IOWIN);
}

static void reg_write(chip_t *c, u32 reg, u32 value) {
    *(volatile u32 *)(c->regs + IOREGSEL) = reg;
    *(volatile u32 *)(c->regs + IOWIN) = value;
}

/* Which controller owns a global interrupt number, and which of its inputs
   that is. A machine with one controller answers the first question
   trivially; a machine with several divides the range between them. */
static chip_t *chip_for(u32 gsi, u32 *input_out) {
    for (u32 i = 0; i < nchips; i++) {
        chip_t *c = &chips[i];
        if (gsi >= c->gsi_base && gsi < c->gsi_base + c->inputs) {
            if (input_out) *input_out = gsi - c->gsi_base;
            return c;
        }
    }
    return 0;
}

static void write_entry(chip_t *c, u32 input, u32 low, u32 high) {
    /* Masked first. The two halves cannot be written at once, and an
       interrupt arriving between them would be delivered using half of the
       old entry and half of the new one. */
    reg_write(c, REG_REDIR + input * 2, ENTRY_MASKED);
    reg_write(c, REG_REDIR + input * 2 + 1, high);
    reg_write(c, REG_REDIR + input * 2, low);
}

u32 ioapic_gsi_for_irq(u8 irq) {
    const acpi_info_t *a = acpi();
    for (u32 i = 0; i < a->noverride; i++)
        if (a->override[i].source == irq) return a->override[i].gsi;
    return irq;                    /* not listed, so wired straight through */
}

static const acpi_override_t *override_for(u8 irq) {
    const acpi_info_t *a = acpi();
    for (u32 i = 0; i < a->noverride; i++)
        if (a->override[i].source == irq) return &a->override[i];
    return 0;
}

bool ioapic_init(void) {
    ready = false;
    nchips = 0;
    memset(chips, 0, sizeof(chips));

    if (!lapic_init()) return false;        /* nothing to acknowledge to */

    const acpi_info_t *a = acpi();
    if (a->nioapic == 0) return false;

    for (u32 i = 0; i < a->nioapic && nchips < ACPI_MAX_IOAPIC; i++) {
        chip_t c;
        c.regs = (volatile u8 *)paging_map_device(a->ioapic[i].address & ~0xFFFull, 0x1000);
        if (!c.regs) continue;
        c.gsi_base = a->ioapic[i].gsi_base;

        /* The version register carries the number of the last input, so the
           count is one more. A controller claiming more than 240 is not one,
           and reading its table would walk off into other registers. */
        u32 ver = reg_read(&c, REG_VER);
        u32 last = (ver >> 16) & 0xFF;
        if (last >= 240) continue;
        c.inputs = last + 1;

        chips[nchips++] = c;

        /* Everything masked. The firmware may have left entries pointing at
           vectors this kernel has not set up, and an interrupt delivered to
           one of those is an exception rather than a handler. */
        for (u32 input = 0; input < c.inputs; input++)
            write_entry(&chips[nchips - 1], input, ENTRY_MASKED, 0);
    }

    if (nchips == 0) return false;
    ready = true;
    return true;
}

bool ioapic_route_irq(u8 irq, u8 vector) {
    if (!ready) return false;

    u32 gsi = ioapic_gsi_for_irq(irq);
    u32 input;
    chip_t *c = chip_for(gsi, &input);
    if (!c) return false;

    const acpi_override_t *o = override_for(irq);
    u32 low = vector | DELIVERY_FIXED | DEST_PHYSICAL;
    if (o && o->active_low)      low |= POLARITY_LOW;
    if (o && o->level_triggered) low |= TRIGGER_LEVEL;

    /* To the boot processor. Spreading interrupts across processors needs a
       scheduler that runs on more than one of them, which this does not
       have yet, so sending them anywhere else would only mean they arrive
       somewhere nothing is waiting. */
    u32 high = (u32)lapic_id() << 24;

    write_entry(c, input, low, high);
    return true;
}

void ioapic_mask_irq(u8 irq) {
    if (!ready) return;
    u32 input;
    chip_t *c = chip_for(ioapic_gsi_for_irq(irq), &input);
    if (!c) return;
    u32 low = reg_read(c, REG_REDIR + input * 2);
    reg_write(c, REG_REDIR + input * 2, low | ENTRY_MASKED);
}

void ioapic_unmask_irq(u8 irq) {
    if (!ready) return;
    u32 input;
    chip_t *c = chip_for(ioapic_gsi_for_irq(irq), &input);
    if (!c) return;
    u32 low = reg_read(c, REG_REDIR + input * 2);
    reg_write(c, REG_REDIR + input * 2, low & ~ENTRY_MASKED);
}
