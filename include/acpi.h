#pragma once
#include "types.h"

/* Just enough ACPI to find the other processors, and where PCIe lives.
 *
 * The firmware describes the machine in a chain of tables: a pointer in low
 * memory leads to a directory of tables, one of which lists every local
 * interrupt controller, which is to say every CPU. There is no other way to
 * find out how many processors exist.
 *
 * Two directories exist. The RSDT holds 32-bit pointers and is what every
 * machine had; the XSDT holds 64-bit ones and is what ACPI 2.0 added in
 * 2000. Firmware that offers both usually agrees between them, so reading
 * only the RSDT worked for a long time. It is not something to rely on: a
 * table sitting above 4 GiB cannot be named by the RSDT at all, and UEFI
 * firmware is entitled to leave the RSDT out entirely. The XSDT is preferred
 * where the revision says there is one, with the RSDT as the fallback.
 *
 * MCFG is the table that says where PCIe configuration space is mapped. See
 * pci.h for what that buys. */

#define ACPI_MAX_CPUS 16
#define ACPI_MAX_MCFG 4
#define ACPI_MAX_IOAPIC 4
#define ACPI_MAX_OVERRIDE 16

/* One contiguous run of buses, and the physical address their configuration
   space starts at. A machine usually has exactly one of these. */
typedef struct {
    u64 base;
    u16 segment;
    u8  start_bus;
    u8  end_bus;
} acpi_mcfg_t;

/* An IOAPIC, and the first global interrupt number it is responsible for.
   A machine with several divides the range between them. */
typedef struct {
    u32 address;
    u32 gsi_base;
    u8  id;
} acpi_ioapic_t;

/* What a legacy IRQ number really arrives as.
 *
 * The eight lines of an XT are wired to whichever IOAPIC inputs the board
 * designer chose, and the firmware lists the ones that are not identity
 * mapped. Almost every machine moves the timer: IRQ 0 comes in on line 2,
 * and a kernel that assumes otherwise routes a line nothing is connected to
 * and then waits forever for a tick that never arrives. */
typedef struct {
    u8  source;                 /* the legacy IRQ number */
    u32 gsi;                    /* the line it is really on */
    bool active_low;
    bool level_triggered;
} acpi_override_t;

typedef struct {
    bool found;                 /* the tables were there and made sense */
    u64  lapic_base;            /* MMIO address of the local APIC */
    u32  ncpus;
    u8   apic_id[ACPI_MAX_CPUS];
    u8   usable[ACPI_MAX_CPUS]; /* the firmware says this one can be started */
    char oem[7];

    u8   revision;              /* 0 means ACPI 1.0, so RSDT only */
    bool used_xsdt;             /* which directory the tables came from */
    u32  ntables;               /* entries in it, whether or not understood */

    u32         nmcfg;
    acpi_mcfg_t mcfg[ACPI_MAX_MCFG];

    u32           nioapic;
    acpi_ioapic_t ioapic[ACPI_MAX_IOAPIC];

    u32             noverride;
    acpi_override_t override[ACPI_MAX_OVERRIDE];

    /* The firmware says the 8259 pair exists and is wired through. When it
       does not, there is nothing to mask and nothing to fall back to. */
    bool has_8259;
} acpi_info_t;

/* Where the firmware said the tables are. Under UEFI this is the only way
   to find them: the root pointer lives in a configuration table the firmware
   hands over, not in the BIOS memory areas, and those areas are not
   guaranteed to contain anything at all on a machine that booted this way.
   Call it before acpi_init; zero means nothing was passed and the low memory
   scan is used instead. */
void acpi_use_rsdp(u64 phys);

bool acpi_init(void);
const acpi_info_t *acpi(void);
