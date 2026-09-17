#pragma once
#include "types.h"

/* PCI configuration space, both ways of reaching it.
 *
 * The old way is a pair of I/O ports: write an address to 0xCF8, read the
 * data at 0xCFC. It works everywhere and reaches the first 256 bytes of a
 * function's configuration space, which is all PCI ever had.
 *
 * PCIe added 4096 bytes per function and did not extend the port pair to
 * reach them, because it cannot: the address written to 0xCF8 has eight bits
 * for the offset and there is nowhere to put a ninth. Everything past 256 is
 * therefore invisible to the legacy mechanism, and that is where the
 * capabilities that matter on modern hardware live. So PCIe maps the whole
 * of configuration space into physical memory instead, and the firmware says
 * where in a table called MCFG.
 *
 * Both are supported and the choice is made once at boot. Offsets below 256
 * go either way; offsets at or above it need the mapping and fail without
 * it, reading as all ones, which is what absent hardware reads as anyway. */

typedef struct {
    u8  bus, slot, func;
    u16 vendor, device;
    u32 bar0;
    u8  irq;
} pci_dev_t;

/* Looks for the MCFG table and maps what it describes. Safe to call with no
   ACPI at all: it simply finds nothing and the port pair stays in use.
   Returns whether extended configuration space is now reachable. */
bool pci_ecam_init(void);
bool pci_ecam_active(void);
u64  pci_ecam_base(void);          /* 0 when inactive */
u8   pci_ecam_last_bus(void);

/* offset is 16 bits because extended configuration space needs 12, and a
   caller asking for one of those on a machine without the mapping should get
   the same answer as asking about hardware that is not there. */
u32  pci_read32(u8 bus, u8 slot, u8 func, u16 offset);
u16  pci_read16(u8 bus, u8 slot, u8 func, u16 offset);
void pci_write32(u8 bus, u8 slot, u8 func, u16 offset, u32 value);
void pci_write16(u8 bus, u8 slot, u8 func, u16 offset, u16 value);
bool pci_find(u16 vendor, u16 device, pci_dev_t *out);
bool pci_find_class(u8 class_code, u8 subclass, u8 prog_if, pci_dev_t *out);

/* Every device of a class and subclass, whatever its programming interface,
   up to max of them. Returns how many there are, which may be more than
   were written.

   For saying what a machine has before there is a driver for it. A laptop
   trackpad that is not on the 8042 is on an I2C controller, and which
   controller decides what writing that driver involves, so a machine that
   cannot use one can at least report that it is there. */
u32  pci_list_class(u8 class_code, u8 subclass, pci_dev_t *out, u32 max);
void pci_enable_bus_master(const pci_dev_t *d);

/* Walks the capability list, returning the offset of the first capability
   with this id, or 0. The extended list lives past 256 and so needs ECAM;
   pci_find_ext_cap returns 0 without it rather than pretending. */
u16 pci_find_cap(const pci_dev_t *d, u8 id);
u16 pci_find_ext_cap(const pci_dev_t *d, u16 id);
