/* See include/lapic.h. */
#include "lapic.h"
#include "acpi.h"
#include "paging.h"

#define LAPIC_ID   0x020
#define LAPIC_SVR  0x0F0          /* spurious interrupt vector register */
#define LAPIC_EOI  0x0B0

static volatile u8 *regs;

volatile u8 *lapic_regs(void) { return regs; }
bool lapic_present(void)      { return regs != 0; }

static void write(u32 reg, u32 value) { *(volatile u32 *)(regs + reg) = value; }
static u32  read(u32 reg)             { return *(volatile u32 *)(regs + reg); }

bool lapic_init(void) {
    if (regs) return true;                  /* already up */

    /* The tables say where it is. There is an architectural default and a
       machine register that also holds the address, but the firmware is
       allowed to move it and the tables are where it says so. */
    if (!acpi_init()) return false;
    const acpi_info_t *a = acpi();
    if (!a->lapic_base) return false;

    /* Above the identity mapped region on every real machine. */
    regs = (volatile u8 *)paging_map_device(a->lapic_base & ~0xFFFull, 0x1000);
    if (!regs) return false;

    /* Bit 8 of the spurious vector register is what switches the thing on.
       The low eight bits are the vector delivered for an interrupt that was
       withdrawn before it could be taken; 0xFF is the conventional choice
       and nothing is registered for it, so it is ignored if it ever fires. */
    write(LAPIC_SVR, read(LAPIC_SVR) | 0x100 | 0xFF);
    return true;
}

/* Switches on the local APIC of the processor that calls it.
 *
 * lapic_init maps the registers and returns early once they are mapped, so
 * the enable bit it writes is written on the boot processor and nowhere
 * else. The address is shared but the register is not: every processor has
 * its own, and one that has never been enabled delivers nothing at all, not
 * even an interrupt sent to it by name. */
void lapic_enable(void) {
    if (!regs) return;
    write(LAPIC_SVR, read(LAPIC_SVR) | 0x100 | 0xFF);
}

u8 lapic_id(void) {
    return regs ? (u8)(read(LAPIC_ID) >> 24) : 0;
}

void lapic_eoi(void) {
    if (regs) write(LAPIC_EOI, 0);
}
