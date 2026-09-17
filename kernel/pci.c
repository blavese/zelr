/* PCI configuration space. See include/pci.h for why there are two ways in.
 *
 * The port pair is kept rather than replaced. It works on every machine
 * including ones with no ACPI at all, it needs nothing mapped, and it is
 * available from the first instruction of the kernel, which matters because
 * the mapped path cannot be set up until paging and ACPI are both running.
 * So the ports are what boots the machine and the mapping takes over for
 * anything that needs to see past the first 256 bytes. */
#include "pci.h"
#include "acpi.h"
#include "paging.h"
#include "io.h"
#include "printf.h"
#include "string.h"

#define CONFIG_ADDR 0xCF8
#define CONFIG_DATA 0xCFC

/* --- the mapped path ------------------------------------------------------ */

static u64 ecam_base;
static u8  ecam_first_bus, ecam_last_bus;
static bool ecam_ready;

/* One bit per bus, because mapping every bus a machine could have means
   256 MiB of page tables for an address space that typically has four buses
   in use. A bus costs a megabyte and is mapped the first time it is read. */
static u32 bus_mapped[8];

static bool bus_is_mapped(u8 bus) { return (bus_mapped[bus >> 5] >> (bus & 31)) & 1; }
static void mark_bus(u8 bus)      { bus_mapped[bus >> 5] |= 1u << (bus & 31); }

bool pci_ecam_init(void) {
    ecam_ready = false;
    ecam_base = 0;
    memset(bus_mapped, 0, sizeof(bus_mapped));

    const acpi_info_t *a = acpi();
    if (!a || a->nmcfg == 0) return false;

    /* Segment zero is the one the rest of this kernel addresses. A machine
       with more than one segment group has buses this cannot name, and
       nothing here would know which segment a caller meant, so the honest
       thing is to use the first group and leave it at that. */
    const acpi_mcfg_t *m = 0;
    for (u32 i = 0; i < a->nmcfg; i++)
        if (a->mcfg[i].segment == 0) { m = &a->mcfg[i]; break; }
    if (!m) return false;

    ecam_base = m->base;
    ecam_first_bus = m->start_bus;
    ecam_last_bus = m->end_bus;
    ecam_ready = true;
    return true;
}

bool pci_ecam_active(void) { return ecam_ready; }
u64  pci_ecam_base(void)   { return ecam_ready ? ecam_base : 0; }
u8   pci_ecam_last_bus(void) { return ecam_ready ? ecam_last_bus : 0; }

/* Where one function's configuration space is, mapping its bus if this is
   the first time anyone has looked at it. Returns 0 when the mapped path
   cannot answer, which sends the caller back to the ports. */
static volatile u8 *ecam_at(u8 bus, u8 slot, u8 func, u16 offset) {
    if (!ecam_ready) return 0;
    if (bus < ecam_first_bus || bus > ecam_last_bus) return 0;
    /* Checked before it is rounded down, or an offset of 0x1000 masks to
       zero and reads the start of the header instead of being refused. */
    if (slot > 31 || func > 7 || offset > 0xFFF) return 0;
    offset &= 0xFFC;

    if (!bus_is_mapped(bus)) {
        u64 bus_base = ecam_base + ((u64)(bus - ecam_first_bus) << 20);
        if (!paging_map_device(bus_base, 1 << 20)) return 0;
        mark_bus(bus);
    }

    return (volatile u8 *)(ecam_base
        + ((u64)(bus - ecam_first_bus) << 20)
        + ((u64)slot << 15)
        + ((u64)func << 12)
        + offset);
}

/* --- the port path -------------------------------------------------------- */

static u32 address(u8 bus, u8 slot, u8 func, u16 offset) {
    return 0x80000000u
         | ((u32)bus  << 16)
         | ((u32)slot << 11)
         | ((u32)func << 8)
         | ((u32)offset & 0xFC);
}

/* --- what everything else calls ------------------------------------------- */

u32 pci_read32(u8 bus, u8 slot, u8 func, u16 offset) {
    volatile u8 *p = ecam_at(bus, slot, func, offset);
    if (p) return *(volatile u32 *)p;

    /* The ports have eight bits of offset and no more, so a request past the
       first 256 bytes has no answer here. All ones is what a read of absent
       hardware returns, and a caller that checks for it at all is already
       checking for this. */
    if (offset >= 256) return 0xFFFFFFFFu;

    outl(CONFIG_ADDR, address(bus, slot, func, offset));
    return inl(CONFIG_DATA);
}

u16 pci_read16(u8 bus, u8 slot, u8 func, u16 offset) {
    u32 v = pci_read32(bus, slot, func, offset);
    return (u16)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_write32(u8 bus, u8 slot, u8 func, u16 offset, u32 value) {
    volatile u8 *p = ecam_at(bus, slot, func, offset);
    if (p) { *(volatile u32 *)p = value; return; }

    if (offset >= 256) return;
    outl(CONFIG_ADDR, address(bus, slot, func, offset));
    outl(CONFIG_DATA, value);
}

void pci_write16(u8 bus, u8 slot, u8 func, u16 offset, u16 value) {
    u32 old = pci_read32(bus, slot, func, offset);
    u32 shift = (offset & 2) * 8;
    u32 v = (old & ~(0xFFFFu << shift)) | ((u32)value << shift);
    pci_write32(bus, slot, func, offset, v);
}

/* --- finding things ------------------------------------------------------- */

/* How far to walk. With a mapping the firmware has said which buses exist,
   and looking past them reads unmapped memory; without one, every bus has to
   be probed because nothing has said otherwise. */
static u16 last_bus(void) {
    return ecam_ready ? (u16)ecam_last_bus : 255;
}

static void fill(pci_dev_t *out, u8 bus, u8 slot, u8 func, u32 id) {
    out->bus = bus;
    out->slot = slot;
    out->func = func;
    out->vendor = (u16)(id & 0xFFFF);
    out->device = (u16)(id >> 16);
    out->bar0 = pci_read32(bus, slot, func, 0x10);
    out->irq  = (u8)(pci_read32(bus, slot, func, 0x3C) & 0xFF);
}

bool pci_find(u16 vendor, u16 device, pci_dev_t *out) {
    u16 end = last_bus();
    for (u16 bus = 0; bus <= end; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                u32 id = pci_read32((u8)bus, slot, func, 0x00);
                u16 v = (u16)(id & 0xFFFF);
                u16 d = (u16)(id >> 16);
                if (v == 0xFFFF) continue;
                if (v == vendor && d == device) {
                    fill(out, (u8)bus, slot, func, id);
                    return true;
                }
                if (func == 0) {
                    u32 hdr = pci_read32((u8)bus, slot, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;   /* not multifunction */
                }
            }
        }
    }
    return false;
}

u32 pci_list_class(u8 class_code, u8 subclass, pci_dev_t *out, u32 max) {
    u16 end = last_bus();
    u32 found = 0;

    for (u16 bus = 0; bus <= end; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                u32 id = pci_read32((u8)bus, slot, func, 0x00);
                if ((u16)(id & 0xFFFF) == 0xFFFF) continue;

                u32 cls = pci_read32((u8)bus, slot, func, 0x08);
                if ((u8)(cls >> 24) != class_code) continue;
                if ((u8)(cls >> 16) != subclass) continue;

                if (out && found < max) fill(&out[found], (u8)bus, slot, func, id);
                found++;
            }
        }
    }
    return found;
}

bool pci_find_class(u8 class_code, u8 subclass, u8 prog_if, pci_dev_t *out) {
    u16 end = last_bus();
    for (u16 bus = 0; bus <= end; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                u32 id = pci_read32((u8)bus, slot, func, 0x00);
                if ((u16)(id & 0xFFFF) == 0xFFFF) continue;

                u32 cls = pci_read32((u8)bus, slot, func, 0x08);
                if ((u8)(cls >> 24) != class_code) continue;
                if ((u8)(cls >> 16) != subclass) continue;
                if ((u8)(cls >> 8) != prog_if) continue;

                fill(out, (u8)bus, slot, func, id);
                return true;
            }
        }
    }
    return false;
}

void pci_enable_bus_master(const pci_dev_t *d) {
    u16 cmd = pci_read16(d->bus, d->slot, d->func, 0x04);
    cmd |= (1 << 0)    /* respond to I/O space */
         | (1 << 1)    /* respond to memory space */
         | (1 << 2);   /* allow it to drive the bus */
    pci_write16(d->bus, d->slot, d->func, 0x04, cmd);
}

/* --- capabilities --------------------------------------------------------- */

/* The list is a chain of offsets inside the first 256 bytes, so a loop over
   it terminates only if the hardware is telling the truth. It is bounded
   here instead: 48 is more entries than the space can hold. */
u16 pci_find_cap(const pci_dev_t *d, u8 id) {
    u16 status = pci_read16(d->bus, d->slot, d->func, 0x06);
    if (!(status & (1 << 4))) return 0;            /* no capability list */

    u8 off = (u8)(pci_read32(d->bus, d->slot, d->func, 0x34) & 0xFC);
    for (int i = 0; i < 48 && off >= 0x40; i++) {
        u32 hdr = pci_read32(d->bus, d->slot, d->func, off);
        if ((u8)(hdr & 0xFF) == id) return off;
        off = (u8)((hdr >> 8) & 0xFC);
        if (!off) break;
    }
    return 0;
}

/* The extended list starts at 256 and is therefore only reachable through
   the mapping. Entries are 16 bit ids with a 12 bit next pointer. */
u16 pci_find_ext_cap(const pci_dev_t *d, u16 id) {
    if (!ecam_ready) return 0;

    u16 off = 0x100;
    for (int i = 0; i < 48 && off >= 0x100 && off <= 0xFFC; i++) {
        u32 hdr = pci_read32(d->bus, d->slot, d->func, off);
        if (hdr == 0 || hdr == 0xFFFFFFFFu) break;
        if ((u16)(hdr & 0xFFFF) == id) return off;
        off = (u16)((hdr >> 20) & 0xFFC);
    }
    return 0;
}
