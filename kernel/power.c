/* Turning the machine off.
 *
 * There is no instruction for this. A modern machine is switched off by
 * asking the chipset for sleep state five, and what has to be written to ask
 * is not fixed: it is two small numbers the firmware chose, kept inside the
 * bytecode of a table, in a named object called _S5.
 *
 * Reading that properly means an AML interpreter, which is a large thing to
 * write for two numbers. What is done here instead is what every small
 * operating system does: find the name in the bytecode and decode the package
 * that follows it, which is a handful of bytes in a shape that has not
 * changed since ACPI 1.0 and that every firmware writes the same way.
 *
 * The honest limit of it is this: a _S5 that is defined conditionally, or
 * built at runtime, or stored somewhere this does not look, will not be
 * found, and then the machine does not turn off and says so rather than
 * pretending. Nothing here writes to the machine on a guess.
 */
#include "power.h"
#include "acpi.h"
#include "paging.h"
#include "io.h"
#include "timer.h"
#include "printf.h"
#include "blackbox.h"

#define SLP_EN      (1u << 13)
#define SCI_EN      (1u << 0)

static bool  looked;
static bool  have_s5;
static u8    slp_typ_a, slp_typ_b;

/* The one AML shape this understands.
 *
 *   '_' 'S' '5' '_'  then a package: 0x12, a length, a count, and the values.
 *
 * A value is either a byte prefix followed by the byte, or one of the two
 * one byte constants for zero and one. Anything else and this gives up,
 * because guessing at bytecode is how a machine gets told to enter a sleep
 * state nobody asked for. */
static bool decode_s5(const u8 *p, const u8 *end) {
    if (p + 8 > end) return false;
    if (*p++ != 0x12) return false;                 /* not a package */

    /* The package length carries its own size in its top two bits. */
    u8 lead = *p;
    p += ((lead >> 6) & 0x03) + 1;
    if (p >= end) return false;

    u8 count = *p++;                                /* elements */
    if (count < 2 || p >= end) return false;

    u8 got[2] = { 0, 0 };
    for (u32 i = 0; i < 2; i++) {
        if (p >= end) return false;
        u8 op = *p++;
        if (op == 0x0A) {                           /* a byte follows */
            if (p >= end) return false;
            got[i] = *p++;
        } else if (op == 0x00) {
            got[i] = 0;
        } else if (op == 0x01) {
            got[i] = 1;
        } else {
            return false;
        }
    }

    slp_typ_a = got[0];
    slp_typ_b = got[1];
    return true;
}

static void find_s5(void) {
    if (looked) return;
    looked = true;

    const acpi_info_t *a = acpi();
    if (!a->fadt.present || !a->fadt.dsdt) return;

    /* The header says how long it is, and that is how much gets searched.
       Reading past it would be reading whatever the firmware left next. */
    const u8 *hdr = (const u8 *)paging_map_device(a->fadt.dsdt, 64);
    if (!hdr) return;
    u32 length = (u32)hdr[4] | ((u32)hdr[5] << 8)
               | ((u32)hdr[6] << 16) | ((u32)hdr[7] << 24);
    if (length < 36 || length > 0x100000) return;

    const u8 *dsdt = (const u8 *)paging_map_device(a->fadt.dsdt, length);
    if (!dsdt) return;

    const u8 *end = dsdt + length;
    for (const u8 *p = dsdt + 36; p + 4 < end; p++) {
        if (p[0] == '_' && p[1] == 'S' && p[2] == '5' && p[3] == '_') {
            /* The name may be preceded by a name path prefix and followed
               directly by the package. Both spellings occur. */
            if (decode_s5(p + 4, end)) { have_s5 = true; return; }
            if (p + 5 < end && decode_s5(p + 5, end)) { have_s5 = true; return; }
        }
    }
}

bool power_can_off(void) {
    find_s5();
    return have_s5 && acpi()->fadt.present;
}

const char *power_describe(void) {
    find_s5();
    if (!acpi()->fadt.present) return "no fixed acpi table";
    if (!have_s5) return "no _S5 in the bytecode";
    return "acpi sleep state 5";
}

bool power_off(void) {
    find_s5();
    const acpi_info_t *a = acpi();
    if (!have_s5 || !a->fadt.present) return false;

    /* Ask the firmware to hand the machine over first. On a machine that
       came up in legacy mode the control register does nothing until it
       has, and on one that is already in ACPI mode there is no SMI command
       to send and the bit is already set. */
    if (a->fadt.smi_cmd && a->fadt.acpi_enable
        && !(inw((u16)a->fadt.pm1a_cnt) & SCI_EN)) {
        outb((u16)a->fadt.smi_cmd, a->fadt.acpi_enable);
        for (u32 i = 0; i < 300; i++) {
            if (inw((u16)a->fadt.pm1a_cnt) & SCI_EN) break;
            sleep_ms(10);
        }
    }

    bb_log("powering off, sleep type %d/%d", slp_typ_a, slp_typ_b);

    outw((u16)a->fadt.pm1a_cnt, (u16)((slp_typ_a << 10) | SLP_EN));
    if (a->fadt.pm1b_cnt)
        outw((u16)a->fadt.pm1b_cnt, (u16)((slp_typ_b << 10) | SLP_EN));

    /* It should not get here. A machine that does is one whose firmware
       wanted something else, and the caller says so rather than this
       looping forever pretending to be asleep. */
    sleep_ms(500);
    return false;
}
