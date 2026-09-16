/* See include/xhci.h.
 *
 * xHCI is not like the controllers zelr already drives. AHCI and NVMe are
 * told where a buffer is and fetch it; this one is told where a *ring* is,
 * and both sides walk it forever. Every ring is an array of sixteen byte
 * blocks with a cycle bit in each, and which entries are live is worked out
 * from that bit rather than from a head and tail pointer: the producer
 * writes entries with the current cycle and flips it each time it wraps, so
 * an entry whose bit matches what the consumer expects is one the producer
 * wrote this time round. The last entry of a command or transfer ring is a
 * Link back to the start, which is what makes a fixed array a ring.
 *
 * There are three kinds:
 *
 *   the command ring    driver to controller: enable a slot, address a
 *                       device, configure an endpoint
 *   the event ring      controller to driver: everything that happened.
 *                       It has no Link; it wraps and the driver flips its
 *                       own expected cycle
 *   a transfer ring     one per endpoint, carrying the actual traffic
 *
 * Nothing here uses an interrupt. The controller can raise one, but an
 * interrupt needs either MSI or a shared PCI line, and a keyboard reporting
 * at most every eight milliseconds is comfortably served by draining the
 * event ring on the timer tick. That also means one consumer for the event
 * ring and no locking anywhere, which is worth more than the latency.
 *
 * Addresses handed to the controller are physical. The heap is inside the
 * identity mapped region, so a pointer from kmalloc is already one; that is
 * checked at startup rather than trusted, the same way nvme.c checks it. */
#include "xhci.h"
#include "pci.h"
#include "paging.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "blackbox.h"

/* --- capability registers, at the base ----------------------------------- */
#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* --- operational registers, at base + CAPLENGTH --------------------------- */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTSC(p)    (0x400u + (u32)(p) * 0x10u)

#define CMD_RUN         (1u << 0)
#define CMD_RESET       (1u << 1)

#define STS_HALTED      (1u << 0)
#define STS_HSE         (1u << 2)
#define STS_CNR         (1u << 11)      /* controller not ready */

/* A port's status and control. */
#define PORT_CCS        (1u << 0)       /* something is plugged in */
#define PORT_PED        (1u << 1)       /* and it is enabled */
#define PORT_RESET      (1u << 4)
#define PORT_POWER      (1u << 9)
#define PORT_SPEED(v)   (((v) >> 10) & 0xF)
#define PORT_CSC        (1u << 17)
#define PORT_PEC        (1u << 18)
#define PORT_WRC        (1u << 19)
#define PORT_OCC        (1u << 20)
#define PORT_PRC        (1u << 21)      /* the reset finished */
#define PORT_PLC        (1u << 22)
#define PORT_CEC        (1u << 23)

/* Those seven are cleared by writing a one to them, and they share a
   register with bits that do something when written. A read-modify-write
   that does not mask them off clears whichever ones happened to be set,
   which throws away the very event being waited for. PED is here too: it is
   write-one-to-*disable*, so carrying it back in turns the port off. */
#define PORT_CHANGES (PORT_CSC | PORT_PEC | PORT_WRC | PORT_OCC | PORT_PRC \
                      | PORT_PLC | PORT_CEC)
#define PORT_KEEP(v) ((v) & ~(PORT_CHANGES | PORT_PED))

/* --- runtime registers, at base + RTSOFF ---------------------------------- */
#define RT_IMAN         0x20            /* interrupter 0 */
#define RT_IMOD         0x24
#define RT_ERSTSZ       0x28
#define RT_ERSTBA       0x30
#define RT_ERDP         0x38
#define ERDP_BUSY       (1u << 3)

/* --- transfer request blocks ---------------------------------------------- */
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_DISABLE_SLOT    10
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_EP    12
#define TRB_EVALUATE_CTX    13
#define TRB_TRANSFER_EVENT  32
#define TRB_CMD_COMPLETE    33
#define TRB_PORT_STATUS     34

#define TRB_CYCLE       (1u << 0)
#define TRB_TOGGLE      (1u << 1)       /* on a Link: flip the cycle here */
#define TRB_CHAIN       (1u << 4)
#define TRB_IOC         (1u << 5)       /* tell me when this one is done */
#define TRB_IDT         (1u << 6)       /* the data is in the TRB itself */
#define TRB_TYPE(t)     ((u32)(t) << 10)
#define TRB_TYPE_OF(c)  (((c) >> 10) & 0x3F)
#define TRB_DIR_IN      (1u << 16)

#define COMP_SUCCESS        1
#define COMP_SHORT_PACKET   13

typedef struct {
    u64 param;
    u32 status;
    u32 control;
} __attribute__((packed)) trb_t;

typedef struct {
    u64 base;
    u32 size;
    u32 reserved;
} __attribute__((packed)) erst_entry_t;

/* A ring the driver writes to. The last entry is always a Link, so there are
   SIZE-1 usable ones. */
#define RING_TRBS   32
typedef struct {
    trb_t *trb;
    u32    at;
    u8     cycle;
} ring_t;

#define EVENT_TRBS  64

/* --- what we know about the controller ------------------------------------ */
static volatile u8 *cap;            /* the mapped registers */
static volatile u8 *op;
static volatile u8 *rt;
static volatile u32 *db;

static u32 nports;
static u32 nslots;
static u32 ctx_stride;              /* 32 or 64 bytes per context */
static bool present;
static char description[72];

static u64 *dcbaa;                  /* one entry per slot, plus scratchpad */
static ring_t cmd_ring;
static trb_t *event_ring;
static erst_entry_t *erst;
static u32 event_at;
static u8  event_cycle;

/* Per slot: its device context and the transfer ring of each endpoint. Only
   the endpoints a keyboard or a mouse uses are ever opened, which is the
   control one and one interrupt-in, but the array is indexed by device
   context index so the numbering matches the specification. */
#define MAX_DCI 8
typedef struct {
    u8     *device_ctx;
    u8     *input_ctx;
    ring_t  ep[MAX_DCI];
    bool    used;
    u32     port;
} slot_t;
static slot_t slots[XHCI_MAX_SLOTS];

/* The last command completion the event ring produced. There is only ever
   one command outstanding, because every one of them is issued by code that
   then waits for it. */
static volatile bool cmd_done;
static volatile u8   cmd_code;
static volatile u8   cmd_slot;

/* And the last transfer completion, per slot and endpoint. */
static volatile u32 xfer_done[XHCI_MAX_SLOTS][MAX_DCI];
static volatile u8  xfer_code[XHCI_MAX_SLOTS][MAX_DCI];
static volatile u32 xfer_left[XHCI_MAX_SLOTS][MAX_DCI];

static void (*report_cb)(u8 slot, u8 dci, u32 len);

/* Set when a root port reports a change and cleared by whoever acts on it. */
static volatile bool port_changed;

/* --- register access ------------------------------------------------------ */
static u32 rd32(volatile u8 *base, u32 off) {
    return *(volatile u32 *)(base + off);
}
static void wr32(volatile u8 *base, u32 off, u32 v) {
    *(volatile u32 *)(base + off) = v;
}
static void wr64(volatile u8 *base, u32 off, u64 v) {
    *(volatile u64 *)(base + off) = v;
}

/* --- memory --------------------------------------------------------------- */
static void *alloc_aligned(u64 bytes, u64 align) {
    u8 *raw = (u8 *)kmalloc(bytes + align);
    if (!raw) return 0;
    u64 addr = ((u64)raw + align - 1) & ~(align - 1);
    memset((void *)addr, 0, bytes);
    return (void *)addr;
}

/* A context is 32 or 64 bytes depending on a bit the controller publishes,
   and the choice is not ours. Everything that indexes one has to go through
   here or it reads the wrong half of the wrong structure on half the
   machines in the world. */
static u32 *ctx_at(u8 *base, u32 index) {
    return (u32 *)(base + index * ctx_stride);
}

/* --- rings ---------------------------------------------------------------- */
/* Allocated the first time and emptied every time after.
 *
 * The heap here hands out an aligned address inside a larger block, so the
 * pointer that comes back is not the one kfree wants and these cannot be
 * freed at all. Keeping them and using them again is what stops a device
 * being unplugged and plugged back in from leaking a ring each time. */
static bool ring_ready(ring_t *r) {
    if (!r->trb) {
        r->trb = (trb_t *)alloc_aligned(RING_TRBS * sizeof(trb_t), 64);
        if (!r->trb) return false;
    } else {
        memset(r->trb, 0, RING_TRBS * sizeof(trb_t));
    }
    r->at = 0;
    r->cycle = 1;
    /* The last entry points back at the first and tells the controller to
       flip its cycle when it follows it, which is what makes the array a
       ring rather than something that runs off the end once. */
    r->trb[RING_TRBS - 1].param = (u64)r->trb;
    r->trb[RING_TRBS - 1].status = 0;
    r->trb[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE;
    return true;
}

static bool ring_init(ring_t *r) { return ring_ready(r); }

/* Puts one TRB on a ring. The cycle bit goes on last, because it is what
   tells the controller the entry is there: writing it first would let the
   controller read a TRB that is still being filled in. */
static void ring_push(ring_t *r, u64 param, u32 status, u32 control) {
    trb_t *t = &r->trb[r->at];
    t->param = param;
    t->status = status;
    __sync_synchronize();
    t->control = control | (r->cycle ? TRB_CYCLE : 0);

    r->at++;
    if (r->at == RING_TRBS - 1) {
        /* Onto the Link, which is written the same way and then followed. */
        trb_t *link = &r->trb[RING_TRBS - 1];
        link->control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE
                      | (r->cycle ? TRB_CYCLE : 0);
        r->at = 0;
        r->cycle ^= 1;
    }
}

static void doorbell(u32 slot, u32 target) {
    db[slot] = target;
    (void)db[slot];                 /* posted writes: make it leave */
}

/* --- waiting -------------------------------------------------------------- */
/*
 * Not with the timer, and not with sleep_ms.
 *
 * All of this runs from main before the scheduler starts, which is also
 * before interrupts are switched on for the first time. sleep_ms halts
 * waiting for a timer interrupt that cannot arrive, and timer_ticks never
 * advances, so a timeout counted in ticks never expires either. Both of them
 * turn a controller that is slow to answer into a machine that never
 * finishes booting, and a machine that never finishes booting is what every
 * laptop would have done, because every laptop has one of these.
 *
 * A write to port 0x80 goes nowhere and takes about a microsecond, on every
 * machine that has ever had an ISA bus behind it. It is not accurate and
 * does not need to be: everything here is waiting for hardware that answers
 * in microseconds, with a limit measured in whole milliseconds.
 */
static void spin_us(u32 us) {
    while (us--) io_wait();
}

static void spin_ms(u32 ms) {
    while (ms--) spin_us(1000);
}

static bool wait_bit(volatile u8 *base, u32 off, u32 mask, bool set, u32 ms) {
    for (u32 i = 0; i < ms * 10 + 2; i++) {
        u32 v = rd32(base, off);
        if (((v & mask) != 0) == set) return true;
        spin_us(100);
    }
    return false;
}

/* --- the event ring ------------------------------------------------------- */
/*
 * One consumer, called both from ordinary code waiting for a command and
 * from the timer interrupt. Ordinary code turns interrupts off around it,
 * so the two can never be inside it at once and neither needs a lock.
 */
static void drain_events(void) {
    if (!present) return;
    for (;;) {
        trb_t *e = &event_ring[event_at];
        if ((e->control & TRB_CYCLE) != event_cycle) break;

        u32 type = TRB_TYPE_OF(e->control);
        u8 code = (u8)(e->status >> 24);

        if (type == TRB_CMD_COMPLETE) {
            cmd_code = code;
            cmd_slot = (u8)(e->control >> 24);
            __sync_synchronize();
            cmd_done = true;
        } else if (type == TRB_TRANSFER_EVENT) {
            u8 slot = (u8)(e->control >> 24);
            u8 dci = (u8)((e->control >> 16) & 0x1F);
            if (slot < XHCI_MAX_SLOTS && dci < MAX_DCI) {
                xfer_code[slot][dci] = code;
                xfer_left[slot][dci] = e->status & 0xFFFFFF;
                __sync_synchronize();
                xfer_done[slot][dci]++;
                /* An interrupt endpoint reports on its own and whoever set
                   it up wants to hear about it now, not next time something
                   asks. The control endpoint is waited on instead, so it is
                   left alone. */
                if (dci > 1 && report_cb
                    && (code == COMP_SUCCESS || code == COMP_SHORT_PACKET)) {
                    report_cb(slot, dci, xfer_left[slot][dci]);
                }
            }
        } else if (type == TRB_PORT_STATUS) {
            /* Something was plugged in or pulled out. Which port it was is
               in the event, but the answer is worked out by looking at all
               of them anyway, and none of it can happen here: enumerating a
               device means control transfers, and this runs inside an
               interrupt handler. So it is recorded and acted on elsewhere. */
            port_changed = true;
        }

        event_at++;
        if (event_at == EVENT_TRBS) {
            event_at = 0;
            event_cycle ^= TRB_CYCLE;
        }
    }
    /* Telling the controller how far we have read also clears its busy bit,
       and it will not raise anything again until that has been done. */
    wr64(rt, RT_ERDP, (u64)&event_ring[event_at] | ERDP_BUSY);
}

void xhci_poll(void) {
    u64 flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags));
    drain_events();
    if (flags & 0x200) __asm__ volatile ("sti");
}

void xhci_on_report(void (*fn)(u8 slot, u8 dci, u32 len)) { report_cb = fn; }

bool xhci_took_port_change(void) {
    if (!port_changed) return false;
    port_changed = false;
    return true;
}

/* --- commands ------------------------------------------------------------- */
/* Issues one command and waits for the controller to finish it. Returns the
   completion code, which is 1 for success and a reason for anything else. */
static u8 command(u64 param, u32 status, u32 control, u8 *slot_out) {
    cmd_done = false;
    ring_push(&cmd_ring, param, status, control);
    doorbell(0, 0);

    /* A second, in hundred microsecond steps. Nothing the controller is
       asked here takes anywhere near that; a command that does has been
       lost rather than been slow. */
    for (u32 i = 0; !cmd_done; i++) {
        xhci_poll();
        if (i > 10000) return 0;                 /* 0 is not a real code */
        spin_us(100);
    }
    if (slot_out) *slot_out = cmd_slot;
    return cmd_code;
}

/* --- bringing the controller up -------------------------------------------- */
static bool reset_controller(void) {
    /* Whatever the firmware was doing with it, stop. */
    wr32(op, OP_USBCMD, rd32(op, OP_USBCMD) & ~CMD_RUN);
    if (!wait_bit(op, OP_USBSTS, STS_HALTED, true, 500)) return false;

    wr32(op, OP_USBCMD, rd32(op, OP_USBCMD) | CMD_RESET);
    /* The reset bit clears itself, and then the controller needs a further
       moment during which every register reads as rubbish. Both waits are
       required and the second one is the one that gets forgotten. */
    if (!wait_bit(op, OP_USBCMD, CMD_RESET, false, 1000)) return false;
    if (!wait_bit(op, OP_USBSTS, STS_CNR, false, 1000)) return false;
    return true;
}

/* Some controllers come out of firmware with the ports routed to a companion
   EHCI controller, and will not see anything plugged in until they are taken
   back. The handover is an extended capability, and asking for it politely
   is all there is to it: set the bit that says the operating system owns
   this, and wait for the firmware's bit to clear. */
static void take_ownership(const pci_dev_t *d) {
    u32 hcc = rd32(cap, CAP_HCCPARAMS1);
    u32 off = (hcc >> 16) & 0xFFFF;
    if (!off) return;
    volatile u8 *p = cap + off * 4;
    for (int guard = 0; guard < 64; guard++) {
        u32 id = *(volatile u32 *)p;
        if ((id & 0xFF) == 1) {                  /* USB legacy support */
            *(volatile u32 *)p = id | (1u << 24);        /* ours now */
            for (int i = 0; i < 100; i++) {
                if (!(*(volatile u32 *)p & (1u << 16))) break;
                spin_ms(10);
            }
            /* And stop it asking for the ports back through SMIs. */
            *(volatile u32 *)(p + 4) = 0;
            return;
        }
        u32 next = (id >> 8) & 0xFF;
        if (!next) return;
        p += next * 4;
    }
    (void)d;
}

static bool make_rings(void) {
    /* One entry per slot, and index zero for the scratchpad array. */
    dcbaa = (u64 *)alloc_aligned((nslots + 1) * sizeof(u64), 64);
    if (!dcbaa) return false;

    /* Scratchpad: pages the controller wants for itself. It says how many,
       and a controller that asked for them and did not get them does not
       work, so this is not optional. */
    u32 hcs2 = rd32(cap, CAP_HCSPARAMS2);
    u32 scratch = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (scratch) {
        u64 *pages = (u64 *)alloc_aligned(scratch * sizeof(u64), 64);
        if (!pages) return false;
        for (u32 i = 0; i < scratch; i++) {
            void *p = alloc_aligned(4096, 4096);
            if (!p) return false;
            pages[i] = (u64)p;
        }
        dcbaa[0] = (u64)pages;
    }

    if (!ring_init(&cmd_ring)) return false;

    event_ring = (trb_t *)alloc_aligned(EVENT_TRBS * sizeof(trb_t), 64);
    erst = (erst_entry_t *)alloc_aligned(sizeof(erst_entry_t), 64);
    if (!event_ring || !erst) return false;
    event_at = 0;
    event_cycle = TRB_CYCLE;
    erst->base = (u64)event_ring;
    erst->size = EVENT_TRBS;
    erst->reserved = 0;

    /* Everything above is handed to the controller as a physical address. */
    if (virt_to_phys((u64)dcbaa) != (u64)dcbaa) return false;
    if (virt_to_phys((u64)cmd_ring.trb) != (u64)cmd_ring.trb) return false;
    if (virt_to_phys((u64)event_ring) != (u64)event_ring) return false;
    return true;
}

bool xhci_init(void) {
    pci_dev_t d;
    /* Serial bus controller, USB, xHCI. */
    if (!pci_find_class(0x0C, 0x03, 0x30, &d)) return false;

    u32 lo = pci_read32(d.bus, d.slot, d.func, 0x10);
    u32 hi = pci_read32(d.bus, d.slot, d.func, 0x14);
    u64 bar = ((u64)hi << 32) | (lo & ~0xFull);
    if (!bar) return false;

    pci_enable_bus_master(&d);
    /* Memory space as well: a controller left with it off answers every
       read with ones, which reads as a controller that is not there. */
    u32 cmdreg = pci_read32(d.bus, d.slot, d.func, 0x04);
    pci_write32(d.bus, d.slot, d.func, 0x04, cmdreg | 0x6);

    cap = (volatile u8 *)paging_map_device(bar, 0x10000);
    if (!cap) return false;

    /* Both of these live in the same first word, and it is read as a word.
       A controller is not obliged to answer a narrower read of its
       capability registers, and one that does not answers with nothing:
       read a halfword at offset two and the version comes back as zero,
       which is how this reported itself as xhci 0.0. */
    u32 first = rd32(cap, 0x00);
    u8 caplen = (u8)(first & 0xFF);
    u16 ver = (u16)(first >> 16);
    op = cap + caplen;
    rt = cap + (rd32(cap, CAP_RTSOFF) & ~0x1Fu);
    db = (volatile u32 *)(cap + (rd32(cap, CAP_DBOFF) & ~0x3u));

    u32 hcs1 = rd32(cap, CAP_HCSPARAMS1);
    nslots = hcs1 & 0xFF;
    nports = (hcs1 >> 24) & 0xFF;
    if (nslots > XHCI_MAX_SLOTS - 1) nslots = XHCI_MAX_SLOTS - 1;
    if (nports > XHCI_MAX_PORTS) nports = XHCI_MAX_PORTS;
    if (!nports) return false;

    ctx_stride = (rd32(cap, CAP_HCCPARAMS1) & (1u << 2)) ? 64 : 32;

    take_ownership(&d);
    if (!reset_controller()) return false;
    if (!make_rings()) return false;

    wr32(op, OP_CONFIG, nslots);
    wr64(op, OP_DCBAAP, (u64)dcbaa);
    /* The ring's address and the cycle the driver is starting on, together,
       because the low bits of this register are not address bits. */
    wr64(op, OP_CRCR, (u64)cmd_ring.trb | 1);

    wr32(rt, RT_ERSTSZ, 1);
    wr64(rt, RT_ERDP, (u64)event_ring | ERDP_BUSY);
    wr64(rt, RT_ERSTBA, (u64)erst);
    wr32(rt, RT_IMOD, 0);

    wr32(op, OP_USBCMD, rd32(op, OP_USBCMD) | CMD_RUN);
    if (!wait_bit(op, OP_USBSTS, STS_HALTED, false, 500)) return false;

    /* Ports come up unpowered on some controllers and nothing is ever seen
       on them until they are told otherwise. */
    for (u32 p = 0; p < nports; p++) {
        u32 sc = rd32(op, OP_PORTSC(p));
        if (!(sc & PORT_POWER))
            wr32(op, OP_PORTSC(p), PORT_KEEP(sc) | PORT_POWER);
    }
    spin_ms(20);

    present = true;
    kformat(description, sizeof description,
              "xhci %d.%d, %d port(s), %d slot(s)",
              (ver >> 8) & 0xFF, (ver >> 4) & 0xF, nports, nslots);
    bb_log("xhci up: %d ports %d slots ctx %d", nports, nslots, ctx_stride);
    return true;
}

bool xhci_present(void) { return present; }
u32  xhci_ports(void) { return present ? nports : 0; }
const char *xhci_describe(void) { return present ? description : "none"; }

bool xhci_port_connected(u32 port) {
    if (!present || port >= nports) return false;
    return (rd32(op, OP_PORTSC(port)) & PORT_CCS) != 0;
}

xhci_speed_t xhci_speed(u32 port) {
    if (!present || port >= nports) return XHCI_SPEED_NONE;
    u32 sc = rd32(op, OP_PORTSC(port));
    if (!(sc & PORT_CCS)) return XHCI_SPEED_NONE;
    return (xhci_speed_t)PORT_SPEED(sc);
}

/* How big a control transfer may be on a device of this speed, before the
   device has been asked. Low speed is always 8, high and super are fixed,
   and full speed is genuinely unknown until its descriptor has been read,
   so 8 is used and corrected afterwards. */
static u16 default_packet(xhci_speed_t s) {
    switch (s) {
    case XHCI_SPEED_SUPER: return 512;
    case XHCI_SPEED_HIGH:  return 64;
    default:               return 8;
    }
}

/* --- attaching a device ---------------------------------------------------- */
static bool reset_port(u32 port) {
    u32 sc = rd32(op, OP_PORTSC(port));
    if (!(sc & PORT_CCS)) return false;

    /* USB 3 ports enable themselves when something is plugged in; USB 2 ones
       have to be reset first, and both are handled by asking for the reset
       and then accepting a port that is already enabled. */
    wr32(op, OP_PORTSC(port), PORT_KEEP(sc) | PORT_RESET);

    for (int i = 0; i < 100; i++) {
        sc = rd32(op, OP_PORTSC(port));
        if (sc & PORT_PRC) {
            /* Acknowledge the change, leaving everything else alone. */
            wr32(op, OP_PORTSC(port), PORT_KEEP(sc) | PORT_PRC);
            break;
        }
        spin_ms(10);
    }
    sc = rd32(op, OP_PORTSC(port));
    return (sc & PORT_PED) != 0;
}

bool xhci_reset_root_port(u32 port) {
    if (!present || port >= nports) return false;
    return reset_port(port);
}

u8 xhci_attach(const xhci_where_t *w) {
    if (!present || !w) return 0;
    xhci_speed_t speed = w->speed;
    if (speed == XHCI_SPEED_NONE) return 0;

    u32 port = w->root_port;
    u8 slot = 0;
    if (command(0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) != COMP_SUCCESS)
        return 0;
    if (!slot || slot >= XHCI_MAX_SLOTS) return 0;

    /* The contexts and the rings are kept from last time this slot was in
       use, for the same reason rings are: nothing here can be freed, so
       everything here is reused. */
    slot_t *s = &slots[slot];
    u8 *device_ctx = s->device_ctx;
    u8 *input_ctx = s->input_ctx;
    trb_t *rings[MAX_DCI];
    for (u32 i = 0; i < MAX_DCI; i++) rings[i] = s->ep[i].trb;

    memset(s, 0, sizeof *s);
    s->device_ctx = device_ctx;
    s->input_ctx = input_ctx;
    for (u32 i = 0; i < MAX_DCI; i++) s->ep[i].trb = rings[i];
    s->port = port;

    /* 32 contexts, at whatever a context is on this controller. */
    if (!s->device_ctx) s->device_ctx = (u8 *)alloc_aligned(32 * ctx_stride, 64);
    if (!s->input_ctx)  s->input_ctx = (u8 *)alloc_aligned(33 * ctx_stride, 64);
    if (!s->device_ctx || !s->input_ctx) return 0;
    memset(s->device_ctx, 0, 32 * ctx_stride);
    memset(s->input_ctx, 0, 33 * ctx_stride);
    if (!ring_ready(&s->ep[1])) return 0;

    dcbaa[slot] = (u64)s->device_ctx;

    /* The input context says what to change. The first context in it is a
       pair of bitmaps: which contexts to drop, and which to add. Adding the
       slot context and endpoint zero is what addressing a device means. */
    u32 *ctrl = ctx_at(s->input_ctx, 0);
    ctrl[1] = 0x3;                                  /* slot and ep0 */

    u32 *sc = ctx_at(s->input_ctx, 1);
    /* The path to it, its speed, and one context below this one meaning
       just endpoint zero. The route is zero for a device on a root port and
       the chain of hub ports for anything below one. */
    sc[0] = (w->route & 0xFFFFF) | ((u32)speed << 20) | (1u << 27);
    sc[1] = (port + 1) << 16;                       /* ports count from one */
    /* And which hub converts for it, if it is a slow device on a fast hub.
       Without this the controller sends at the device's speed through a hub
       that is not expecting it, and nothing answers. */
    sc[2] = (u32)w->tt_slot | ((u32)w->tt_port << 8);

    u32 *ep0 = ctx_at(s->input_ctx, 2);
    ep0[1] = (4u << 3)                              /* control, both ways */
           | (3u << 1)                              /* three errors allowed */
           | ((u32)default_packet(speed) << 16);
    ep0[2] = (u32)((u64)s->ep[1].trb | 1);          /* and the cycle we start on */
    ep0[3] = (u32)((u64)s->ep[1].trb >> 32);
    ep0[4] = 8;                                     /* average transfer */

    s->used = true;
    if (command((u64)s->input_ctx, 0,
                TRB_TYPE(TRB_ADDRESS_DEVICE) | ((u32)slot << 24), 0)
            != COMP_SUCCESS) {
        s->used = false;
        return 0;
    }
    return slot;
}

/* See include/xhci.h. The fields live in the slot context, and Configure
   Endpoint with only the slot context named is what applies them.
 *
 * Nothing here is covered by the tests. QEMU routes to a device behind a hub
 * whether or not it has been told the hub is a hub, so the usb harness passes
 * with the hub bit deliberately left clear, which was checked. Real
 * controllers use it to decide how to reach anything below this slot, so it
 * is set because the specification says to and not because anything here
 * proves it. */
bool xhci_mark_hub(u8 slot, u8 ports, u8 think_time, bool multi_tt) {
    if (!present || slot >= XHCI_MAX_SLOTS || !slots[slot].used) return false;
    slot_t *s = &slots[slot];

    memset(s->input_ctx, 0, 33 * ctx_stride);
    u32 *ctrl = ctx_at(s->input_ctx, 0);
    ctrl[1] = 0x1;                                  /* the slot context only */

    memcpy(ctx_at(s->input_ctx, 1), ctx_at(s->device_ctx, 0), ctx_stride);
    u32 *sc = ctx_at(s->input_ctx, 1);
    sc[0] |= (1u << 26);                            /* this is a hub */
    if (multi_tt) sc[0] |= (1u << 25);
    sc[1] = (sc[1] & 0x00FFFFFFu) | ((u32)ports << 24);
    sc[2] = (sc[2] & ~(3u << 16)) | (((u32)think_time & 3) << 16);

    return command((u64)s->input_ctx, 0,
                   TRB_TYPE(TRB_CONFIGURE_EP) | ((u32)slot << 24), 0)
           == COMP_SUCCESS;
}

void xhci_detach(u8 slot) {
    if (!present || slot >= XHCI_MAX_SLOTS || !slots[slot].used) return;
    command(0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((u32)slot << 24), 0);
    slots[slot].used = false;
    dcbaa[slot] = 0;
}

/* --- control transfers ------------------------------------------------------ */
bool xhci_control(u8 slot, const usb_setup_t *setup, void *data, u16 len) {
    if (!present || slot >= XHCI_MAX_SLOTS || !slots[slot].used) return false;
    ring_t *r = &slots[slot].ep[1];
    bool in = (setup->type & 0x80) != 0;

    if (data && len && virt_to_phys((u64)data) != (u64)data) return false;

    u32 before = xfer_done[slot][1];

    /* Setup: the eight bytes travel inside the TRB itself rather than being
       pointed at, which is what the immediate-data bit means. */
    u64 packet;
    memcpy(&packet, setup, 8);
    u32 trt = len ? (in ? 3u : 2u) : 0u;
    ring_push(r, packet, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));

    if (data && len)
        ring_push(r, (u64)data, len,
                  TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0));

    /* The status stage goes the other way from the data, and is the one
       asked to report, because it is the end of the whole transfer. */
    ring_push(r, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (in ? 0 : TRB_DIR_IN));

    doorbell(slot, 1);

    for (u32 i = 0; xfer_done[slot][1] == before; i++) {
        xhci_poll();
        if (i > 10000) return false;
        spin_us(100);
    }
    u8 code = xfer_code[slot][1];
    return code == COMP_SUCCESS || code == COMP_SHORT_PACKET;
}

bool xhci_set_packet_size(u8 slot, u16 max_packet) {
    if (!present || slot >= XHCI_MAX_SLOTS || !slots[slot].used) return false;
    if (!max_packet) return false;
    slot_t *s = &slots[slot];

    /* Evaluate Context changes a field of an endpoint that is already set
       up, without tearing it down. Only endpoint zero is named, so only its
       packet size is looked at. */
    memset(s->input_ctx, 0, 33 * ctx_stride);
    u32 *ctrl = ctx_at(s->input_ctx, 0);
    ctrl[1] = 0x2;                                  /* endpoint zero only */

    u32 *ep0 = ctx_at(s->input_ctx, 2);
    memcpy(ep0, ctx_at(s->device_ctx, 1), ctx_stride);
    ep0[1] = (ep0[1] & 0xFFFFu) | ((u32)max_packet << 16);

    return command((u64)s->input_ctx, 0,
                   TRB_TYPE(TRB_EVALUATE_CTX) | ((u32)slot << 24), 0)
           == COMP_SUCCESS;
}

/* --- interrupt endpoints ----------------------------------------------------- */
bool xhci_open_interrupt_in(u8 slot, u8 dci, u16 max_packet, u8 interval) {
    if (!present || slot >= XHCI_MAX_SLOTS || !slots[slot].used) return false;
    if (dci < 2 || dci >= MAX_DCI) return false;
    slot_t *s = &slots[slot];
    if (!ring_ready(&s->ep[dci])) return false;

    memset(s->input_ctx, 0, 33 * ctx_stride);
    u32 *ctrl = ctx_at(s->input_ctx, 0);
    ctrl[1] = 1u | (1u << dci);           /* the slot context, and this one */

    /* The slot context is copied from the device and told it now has more
       endpoints below it, which is the number the controller uses to decide
       how much of the context to look at. */
    memcpy(ctx_at(s->input_ctx, 1), ctx_at(s->device_ctx, 0), ctx_stride);
    u32 *sc = ctx_at(s->input_ctx, 1);
    sc[0] = (sc[0] & ~(0x1Fu << 27)) | ((u32)dci << 27);

    u32 *ep = ctx_at(s->input_ctx, dci + 1);
    ep[0] = (u32)interval << 16;
    ep[1] = (7u << 3)                     /* interrupt, inward */
          | (3u << 1)
          | ((u32)max_packet << 16);
    ep[2] = (u32)((u64)s->ep[dci].trb | 1);
    ep[3] = (u32)((u64)s->ep[dci].trb >> 32);
    ep[4] = max_packet | ((u32)max_packet << 16);

    return command((u64)s->input_ctx, 0,
                   TRB_TYPE(TRB_CONFIGURE_EP) | ((u32)slot << 24), 0)
           == COMP_SUCCESS;
}

bool xhci_listen(u8 slot, u8 dci, void *buf, u16 len) {
    if (!present || slot >= XHCI_MAX_SLOTS || !slots[slot].used) return false;
    if (dci < 2 || dci >= MAX_DCI) return false;
    if (virt_to_phys((u64)buf) != (u64)buf) return false;
    ring_push(&slots[slot].ep[dci], (u64)buf, len,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    doorbell(slot, dci);
    return true;
}
