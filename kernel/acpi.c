/* Finding the other processors.
 *
 * A PC does not tell you how many CPUs it has; you have to go and look. The
 * firmware leaves a pointer in low memory, that leads to a directory of
 * tables, and one of those tables lists every local interrupt controller.
 * Each of those is a processor.
 *
 * Everything here is read-only and happens once, at boot. The tables can sit
 * anywhere in physical memory, including above the region the kernel
 * identity maps, so each one is mapped before it is read. */
#include "acpi.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"

typedef struct {
    char sig[8];               /* "RSD PTR " */
    u8   checksum;
    char oem[6];
    u8   revision;
    u32  rsdt_address;
    /* revision 2 adds a length, an xsdt address and a second checksum */
    u32  length;
    u64  xsdt_address;
    u8   ext_checksum;
    u8   reserved[3];
} __attribute__((packed)) rsdp_t;

typedef struct {
    char sig[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem[6];
    char oem_table[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} __attribute__((packed)) sdt_header_t;

/* Multiple APIC Description Table: the header, then a stream of entries of
   differing kinds, each carrying its own length. */
typedef struct {
    sdt_header_t header;
    u32 lapic_address;
    u32 flags;
} __attribute__((packed)) madt_t;

#define MADT_LAPIC          0
#define MADT_IOAPIC         1
#define MADT_OVERRIDE       2
#define MADT_LAPIC_OVERRIDE 5

/* Memory mapped configuration space: the header, eight reserved bytes, then
   one sixteen byte entry per run of buses. */
typedef struct {
    u64 base;
    u16 segment;
    u8  start_bus;
    u8  end_bus;
    u32 reserved;
} __attribute__((packed)) mcfg_entry_t;

static acpi_info_t info;

const acpi_info_t *acpi(void) { return &info; }

/* Makes a physical range readable. Below the identity mapped region there is
   nothing to do; above it, the pages are mapped where they already are, so
   the pointer the caller gets is the physical address either way. */
static const void *map_phys(u64 phys, u64 len) {
    /* Asked of the page tables rather than compared against a constant.
       What is identity mapped is now whatever the firmware said is memory,
       so there is no single line below which the answer is always yes. */
    if (virt_to_phys(phys) == phys
        && virt_to_phys(phys + len - 1) == phys + len - 1)
        return (const void *)phys;

    u64 first = phys & ~0xFFFull;
    u64 last = (phys + len + PAGE_SIZE - 1) & ~0xFFFull;
    for (u64 a = first; a < last; a += PAGE_SIZE)
        if (!virt_to_phys(a) && !map_page(a, a, PTE_PRESENT | PTE_RW)) return 0;
    return (const void *)phys;
}

static bool checksum_ok(const u8 *p, u32 len) {
    u8 sum = 0;
    for (u32 i = 0; i < len; i++) sum = (u8)(sum + p[i]);
    return sum == 0;
}

/* What the bootloader was told, if it was told anything. */
static u64 given_rsdp;

void acpi_use_rsdp(u64 phys) { given_rsdp = phys; }

/* The pointer is either where the firmware said it is, or, on a machine that
   booted through a BIOS, in the first kilobyte of the extended BIOS data area
   or somewhere in the last 128 KiB below a megabyte, on a 16 byte boundary
   in both cases.
 *
 * The scan is not a substitute for being told. UEFI firmware is under no
 * obligation to leave anything in those addresses, and on the machines where
 * it does not, a kernel that only scans finds no tables, which reads as a
 * machine with one processor and no PCIe rather than as the failure it is. */
static const rsdp_t *find_rsdp(void) {
    if (given_rsdp) {
        const rsdp_t *r = (const rsdp_t *)map_phys(given_rsdp, sizeof(rsdp_t));
        if (r && memcmp(r->sig, "RSD PTR ", 8) == 0 && checksum_ok((const u8 *)r, 20))
            return r;
        /* Told, but wrong. Fall through and look: better than giving up. */
    }

    u64 ebda = (u64)(*(volatile u16 *)0x40E) << 4;
    if (ebda >= 0x400 && ebda < 0xA0000) {
        for (u64 a = ebda; a < ebda + 1024; a += 16) {
            const rsdp_t *r = (const rsdp_t *)a;
            if (memcmp(r->sig, "RSD PTR ", 8) == 0 && checksum_ok((const u8 *)r, 20))
                return r;
        }
    }
    for (u64 a = 0xE0000; a < 0x100000; a += 16) {
        const rsdp_t *r = (const rsdp_t *)a;
        if (memcmp(r->sig, "RSD PTR ", 8) == 0 && checksum_ok((const u8 *)r, 20))
            return r;
    }
    return 0;
}

static void read_madt(const madt_t *madt) {
    info.lapic_base = madt->lapic_address;
    /* Bit 0 of the flags says the machine has the 8259 pair wired up and
       that it must be masked before the IOAPIC is used, or both will
       deliver the same interrupt. */
    info.has_8259 = (madt->flags & 1) != 0;

    u32 len = madt->header.length;
    const u8 *p = (const u8 *)madt + sizeof(madt_t);
    const u8 *end = (const u8 *)madt + len;

    while (p + 2 <= end) {
        u8 type = p[0], entry_len = p[1];
        if (entry_len < 2) break;                 /* malformed; stop */
        if (p + entry_len > end) break;

        if (type == MADT_LAPIC && entry_len >= 8) {
            u8 apic_id = p[3];
            u32 flags = *(const u32 *)(p + 4);
            if (info.ncpus < ACPI_MAX_CPUS) {
                info.apic_id[info.ncpus] = apic_id;
                /* Bit 0 says it is enabled; bit 1 says it could be brought
                   online later. Either is worth trying. */
                info.usable[info.ncpus] = (flags & 0x3) ? 1 : 0;
                info.ncpus++;
            }
        } else if (type == MADT_IOAPIC && entry_len >= 12) {
            if (info.nioapic < ACPI_MAX_IOAPIC) {
                acpi_ioapic_t *io = &info.ioapic[info.nioapic];
                io->id       = p[2];
                io->address  = *(const u32 *)(p + 4);
                io->gsi_base = *(const u32 *)(p + 8);
                if (io->address) info.nioapic++;
            }
        } else if (type == MADT_OVERRIDE && entry_len >= 10) {
            if (info.noverride < ACPI_MAX_OVERRIDE) {
                acpi_override_t *o = &info.override[info.noverride];
                u16 flags = *(const u16 *)(p + 8);
                o->source = p[3];
                o->gsi    = *(const u32 *)(p + 4);
                /* Two bits each, and zero in either means "whatever the bus
                   normally does", which for the ISA bus is active high and
                   edge triggered. */
                o->active_low       = (flags & 0x3) == 0x3;
                o->level_triggered  = ((flags >> 2) & 0x3) == 0x3;
                info.noverride++;
            }
        } else if (type == MADT_LAPIC_OVERRIDE && entry_len >= 12) {
            /* A 64 bit address, but the low half is what a 32 bit kernel can
               reach, and no real machine puts it above 4 GiB. */
            info.lapic_base = *(const u32 *)(p + 4);
        }

        p += entry_len;
    }
}

static void read_mcfg(const sdt_header_t *h) {
    u32 len = h->length;
    if (len < sizeof(sdt_header_t) + 8) return;

    const u8 *p = (const u8 *)h + sizeof(sdt_header_t) + 8;
    const u8 *end = (const u8 *)h + len;

    while (p + sizeof(mcfg_entry_t) <= end && info.nmcfg < ACPI_MAX_MCFG) {
        const mcfg_entry_t *e = (const mcfg_entry_t *)p;

        /* A run has to be the right way round and land somewhere. Firmware
           gets this wrong often enough that a driver built on an unchecked
           entry would be reading from address zero on those machines. */
        if (e->base && e->start_bus <= e->end_bus) {
            info.mcfg[info.nmcfg].base      = e->base;
            info.mcfg[info.nmcfg].segment   = e->segment;
            info.mcfg[info.nmcfg].start_bus = e->start_bus;
            info.mcfg[info.nmcfg].end_bus   = e->end_bus;
            info.nmcfg++;
        }
        p += sizeof(mcfg_entry_t);
    }
}

/* One entry of whichever directory was used. Both kinds are a physical
   address of a table; they differ only in how wide that address is. */
static void read_fadt(const sdt_header_t *h);

static void read_table(u64 phys) {
    const sdt_header_t *h = (const sdt_header_t *)map_phys(phys, sizeof(sdt_header_t));
    if (!h) return;
    if (h->length < sizeof(sdt_header_t) || h->length > 0x10000) return;
    if (!map_phys(phys, h->length)) return;
    if (!checksum_ok((const u8 *)h, h->length)) return;

    if (memcmp(h->sig, "APIC", 4) == 0 && h->length >= sizeof(madt_t))
        read_madt((const madt_t *)h);
    else if (memcmp(h->sig, "MCFG", 4) == 0)
        read_mcfg(h);
    else if (memcmp(h->sig, "FACP", 4) == 0)
        read_fadt(h);
}

/* Little endian fields out of a byte array, because the tables are not
   aligned to anything and the offsets into them are given in bytes. */
static u32 le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 le64(const u8 *p) {
    return (u64)le32(p) | ((u64)le32(p + 4) << 32);
}

/* The fixed table. Its signature is FACP and its name is FADT, for reasons
   that made sense to somebody in 1996.
 *
 * Only four things are taken from it, and all four are about turning the
 * machine off: the control register, its second half on machines that have
 * one, the port that asks the firmware to hand over ACPI mode, and where the
 * bytecode lives. The offsets are fixed and have been since ACPI 1.0. */
static void read_fadt(const sdt_header_t *h) {
    if (h->length < 90) return;
    const u8 *p = (const u8 *)h;

    u32 dsdt32   = le32(p + 40);
    info.fadt.smi_cmd     = le32(p + 48);
    info.fadt.acpi_enable = p[52];
    info.fadt.pm1a_cnt    = le32(p + 64);
    info.fadt.pm1b_cnt    = le32(p + 68);

    /* The sixty four bit pointer, on anything that has one. A machine with
       its tables above four gigabytes has nothing useful in the old field. */
    info.fadt.dsdt = dsdt32;
    if (h->revision >= 2 && h->length >= 148) {
        u64 x = le64(p + 140);
        if (x) info.fadt.dsdt = x;
    }

    info.fadt.present = info.fadt.pm1a_cnt != 0;
}

/* The tables are read once. Two callers want them and the order they run in
   is not theirs to decide: the PCIe mapping has to exist before the disk and
   network drivers probe, and bringing up the other processors happens well
   after that. Whichever asks first does the work. */
static bool acpi_ran;

bool acpi_init(void) {
    if (acpi_ran) return info.found;
    acpi_ran = true;

    memset(&info, 0, sizeof(info));
    info.lapic_base = 0xFEE00000;                 /* the architectural default */

    const rsdp_t *rsdp = find_rsdp();
    if (!rsdp) return false;

    memcpy(info.oem, rsdp->oem, 6);
    info.oem[6] = 0;
    info.revision = rsdp->revision;

    /* Revision 2 and above carry a second checksum over the whole structure,
       and only then are the length and the xsdt address part of it. Reading
       those fields without checking it means trusting bytes that an ACPI 1.0
       firmware never wrote. */
    bool xsdt_ok = false;
    if (rsdp->revision >= 2 && rsdp->length >= sizeof(rsdp_t) &&
        checksum_ok((const u8 *)rsdp, rsdp->length) && rsdp->xsdt_address)
        xsdt_ok = true;

    u64 dir_phys = xsdt_ok ? rsdp->xsdt_address : (u64)rsdp->rsdt_address;
    const char *want = xsdt_ok ? "XSDT" : "RSDT";
    u32 stride = xsdt_ok ? 8u : 4u;

    const sdt_header_t *dir =
        (const sdt_header_t *)map_phys(dir_phys, sizeof(sdt_header_t));

    /* Firmware that claims an XSDT and then does not provide a usable one is
       rare but not unheard of, so fall back rather than give up. */
    if ((!dir || memcmp(dir->sig, want, 4) != 0) && xsdt_ok) {
        xsdt_ok = false;
        dir_phys = rsdp->rsdt_address;
        want = "RSDT";
        stride = 4;
        dir = (const sdt_header_t *)map_phys(dir_phys, sizeof(sdt_header_t));
    }
    if (!dir || memcmp(dir->sig, want, 4) != 0) return false;

    u32 length = dir->length;
    if (length < sizeof(sdt_header_t) || length > 0x10000) return false;
    if (!map_phys(dir_phys, length)) return false;
    if (!checksum_ok((const u8 *)dir, length)) return false;

    info.used_xsdt = xsdt_ok;

    u32 count = (length - sizeof(sdt_header_t)) / stride;
    const u8 *entries = (const u8 *)dir + sizeof(sdt_header_t);
    info.ntables = count;

    /* Every entry, not the first interesting one: the MADT and the MCFG are
       both wanted and nothing says which comes first. */
    for (u32 i = 0; i < count; i++) {
        u64 phys = xsdt_ok ? *(const u64 *)(entries + i * 8)
                           : (u64)*(const u32 *)(entries + i * 4);
        if (phys) read_table(phys);
    }

    info.found = info.ncpus > 0;
    return info.found;
}
