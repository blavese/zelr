/* See include/nvme.h.
 *
 * Everything here is polled. The controller can raise an interrupt when a
 * completion is posted, and eventually it should, but the block layer above
 * is synchronous: it asks for sectors and waits for them. An interrupt would
 * arrive, set a flag, and be waited on by the same loop that is written here
 * without it, so the interrupt buys nothing until there is something else
 * for the processor to do meanwhile.
 *
 * The addresses handed to the controller are physical, and the kernel's heap
 * lives in the identity mapped region, so a pointer from kmalloc is already
 * one. That assumption is shared with the AHCI driver and is checked at
 * startup rather than trusted. */
#include "nvme.h"
#include "pci.h"
#include "paging.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "blockdev.h"

/* Controller registers, as offsets from the mapped base. */
#define REG_CAP     0x00        /* 64 bit */
#define REG_VS      0x08
#define REG_CC      0x14
#define REG_CSTS    0x1C
#define REG_AQA     0x24
#define REG_ASQ     0x28        /* 64 bit */
#define REG_ACQ     0x30        /* 64 bit */
#define REG_DOORBELL 0x1000

#define CC_ENABLE   (1u << 0)
#define CSTS_READY  (1u << 0)
#define CSTS_FATAL  (1u << 1)

/* Admin commands. */
#define ADM_CREATE_SQ   0x01
#define ADM_CREATE_CQ   0x05
#define ADM_IDENTIFY    0x06

/* I/O commands. */
#define IO_FLUSH        0x00
#define IO_WRITE        0x01
#define IO_READ         0x02

#define QUEUE_ENTRIES   64      /* both queues; far more than is ever in flight */
#define IO_QID          1

/* One page per transfer, so a command never needs more than its first
   address. The list that would otherwise be required is a second allocation
   and a second failure mode, for a driver whose caller splits anyway. */
#define MAX_SECTORS_PER_RUN 8   /* 4096 bytes */

typedef struct {
    u8  opcode;
    u8  flags;
    u16 cid;
    u32 nsid;
    u64 reserved;
    u64 metadata;
    u64 prp1;
    u64 prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed)) sqe_t;

typedef struct {
    u32 result;
    u32 reserved;
    u16 sq_head;
    u16 sq_id;
    u16 cid;
    u16 status;             /* phase in bit 0, the code above it */
} __attribute__((packed)) cqe_t;

typedef struct {
    sqe_t *sq;
    cqe_t *cq;
    u16    sq_tail;
    u16    cq_head;
    u8     phase;           /* flips each time the completion queue wraps */
    u16    qid;
} queue_t;

static volatile u8 *regs;
static queue_t admin, io;
static u8  *buffer;             /* one page, aligned, for every transfer */
static u32 doorbell_stride;
static u32 timeout_ms;
static u32 total_sectors;
static u32 sector_shift = 9;
static bool present;
static char model[41];
static u16 next_cid = 1;

bool nvme_present(void)   { return present; }
u32  nvme_sectors(void)   { return total_sectors; }
u32  nvme_max_run(void)   { return MAX_SECTORS_PER_RUN; }
const char *nvme_model(void) { return model; }

/* --- registers ------------------------------------------------------------ */

static u32 rd32(u32 off)            { return *(volatile u32 *)(regs + off); }
static void wr32(u32 off, u32 v)    { *(volatile u32 *)(regs + off) = v; }
static u64 rd64(u32 off) {
    /* Two halves rather than one access: a 64 bit MMIO read is not something
       every chipset handles, and the low half has to be read first. */
    u32 lo = rd32(off), hi = rd32(off + 4);
    return ((u64)hi << 32) | lo;
}
static void wr64(u32 off, u64 v) {
    wr32(off, (u32)v);
    wr32(off + 4, (u32)(v >> 32));
}

/* Doorbells are spaced by a stride the controller reports, because a device
   may want each one in its own cache line. Assuming they are adjacent works
   on most hardware and silently rings the wrong bell on the rest. */
static u32 doorbell(u16 qid, bool completion) {
    return REG_DOORBELL + ((u32)qid * 2 + (completion ? 1 : 0)) * doorbell_stride;
}

/* --- memory --------------------------------------------------------------- */

static void *alloc_aligned(u64 bytes, u64 align) {
    u8 *raw = (u8 *)kmalloc(bytes + align);
    if (!raw) return 0;
    u64 addr = ((u64)raw + align - 1) & ~(align - 1);
    memset((void *)addr, 0, bytes);
    return (void *)addr;
}

/* --- commands ------------------------------------------------------------- */

/* Submits one command and waits for its completion.
 *
 * The completion queue has no flag saying an entry is new; instead every
 * entry carries a phase bit, the controller writes the current phase, and
 * the driver flips what it expects each time it wraps. An entry whose phase
 * matches is one the controller has written this time around. */
static bool submit(queue_t *q, sqe_t *cmd) {
    if (!regs) return false;

    cmd->cid = next_cid++;
    if (next_cid == 0) next_cid = 1;

    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (u16)((q->sq_tail + 1) % QUEUE_ENTRIES);
    wr32(doorbell(q->qid, false), q->sq_tail);

    /* The controller publishes how long it may take to become ready; a
       command that outlives that has not been slow, it has been lost. */
    u64 deadline = timer_ticks() + (timeout_ms / 10) + 2;
    for (;;) {
        cqe_t *e = &q->cq[q->cq_head];
        if ((e->status & 1) == q->phase) {
            u16 code = (u16)(e->status >> 1);
            q->cq_head = (u16)((q->cq_head + 1) % QUEUE_ENTRIES);
            if (q->cq_head == 0) q->phase ^= 1;
            wr32(doorbell(q->qid, true), q->cq_head);
            return code == 0;
        }
        if (rd32(REG_CSTS) & CSTS_FATAL) return false;
        if (timer_ticks() > deadline) return false;
        __asm__ volatile ("pause");
    }
}

static bool wait_ready(bool want) {
    u64 deadline = timer_ticks() + (timeout_ms / 10) + 2;
    for (;;) {
        u32 csts = rd32(REG_CSTS);
        if (csts & CSTS_FATAL) return false;
        if (((csts & CSTS_READY) != 0) == want) return true;
        if (timer_ticks() > deadline) return false;
        __asm__ volatile ("pause");
    }
}

/* --- bringing it up -------------------------------------------------------- */

static bool make_queues(void) {
    admin.sq = (sqe_t *)alloc_aligned(QUEUE_ENTRIES * sizeof(sqe_t), 4096);
    admin.cq = (cqe_t *)alloc_aligned(QUEUE_ENTRIES * sizeof(cqe_t), 4096);
    io.sq    = (sqe_t *)alloc_aligned(QUEUE_ENTRIES * sizeof(sqe_t), 4096);
    io.cq    = (cqe_t *)alloc_aligned(QUEUE_ENTRIES * sizeof(cqe_t), 4096);
    buffer   = (u8 *)alloc_aligned(4096, 4096);
    if (!admin.sq || !admin.cq || !io.sq || !io.cq || !buffer) return false;

    /* The controller is given these as physical addresses. Everything the
       heap hands out is inside the identity mapped region, so the two are
       the same number; if that ever stops being true, this is where it goes
       wrong, so it is checked rather than assumed. */
    if (virt_to_phys((u64)admin.sq) != (u64)admin.sq) return false;
    if (virt_to_phys((u64)io.sq) != (u64)io.sq) return false;
    if (virt_to_phys((u64)buffer) != (u64)buffer) return false;

    admin.qid = 0; admin.phase = 1; admin.sq_tail = 0; admin.cq_head = 0;
    io.qid = IO_QID; io.phase = 1; io.sq_tail = 0; io.cq_head = 0;
    return true;
}

static bool identify(u32 cns, u32 nsid, void *out) {
    sqe_t c;
    memset(&c, 0, sizeof(c));
    c.opcode = ADM_IDENTIFY;
    c.nsid = nsid;
    c.prp1 = (u64)buffer;
    c.cdw10 = cns;
    if (!submit(&admin, &c)) return false;
    memcpy(out, buffer, 4096);
    return true;
}

static bool create_io_queues(void) {
    sqe_t c;

    /* The completion queue first: a submission queue names the completion
       queue its results go to, so the other order describes one that does
       not exist yet. */
    memset(&c, 0, sizeof(c));
    c.opcode = ADM_CREATE_CQ;
    c.prp1 = (u64)io.cq;
    c.cdw10 = ((QUEUE_ENTRIES - 1) << 16) | IO_QID;
    c.cdw11 = 1;                       /* physically contiguous, no interrupts */
    if (!submit(&admin, &c)) return false;

    memset(&c, 0, sizeof(c));
    c.opcode = ADM_CREATE_SQ;
    c.prp1 = (u64)io.sq;
    c.cdw10 = ((QUEUE_ENTRIES - 1) << 16) | IO_QID;
    c.cdw11 = (IO_QID << 16) | 1;      /* contiguous, completions to queue 1 */
    return submit(&admin, &c);
}

bool nvme_init(void) {
    present = false;
    total_sectors = 0;
    regs = 0;
    memset(model, 0, sizeof(model));

    pci_dev_t dev;
    /* Class 01 subclass 08 programming interface 02 is what the
       specification assigns, so any vendor's controller is found alike. */
    if (!pci_find_class(0x01, 0x08, 0x02, &dev)) return false;

    pci_enable_bus_master(&dev);

    /* A 64 bit bar, so the upper half is in the next slot. Ignoring it works
       until the firmware puts the controller above four gigabytes, which is
       common on a machine with a lot of memory. */
    u32 bar_lo = pci_read32(dev.bus, dev.slot, dev.func, 0x10);
    u32 bar_hi = pci_read32(dev.bus, dev.slot, dev.func, 0x14);
    u64 base = ((u64)bar_hi << 32) | (bar_lo & 0xFFFFFFF0u);
    if (!base) return false;

    regs = (volatile u8 *)paging_map_device(base, 0x2000);
    if (!regs) return false;

    u64 cap = rd64(REG_CAP);
    doorbell_stride = 4u << ((cap >> 32) & 0xF);
    timeout_ms = (u32)(((cap >> 24) & 0xFF) * 500);
    if (timeout_ms == 0 || timeout_ms > 30000) timeout_ms = 30000;

    u32 max_entries = (u32)(cap & 0xFFFF) + 1;
    if (max_entries < QUEUE_ENTRIES) return false;

    /* Bit 37 says whether the NVM command set is supported at all. A
       controller that only speaks something else is not a disk to us. */
    if (!((cap >> 37) & 1)) return false;

    /* Off before anything is configured. Writing the queue registers while
       it is running is not allowed and not checked by the hardware. */
    wr32(REG_CC, rd32(REG_CC) & ~CC_ENABLE);
    if (!wait_ready(false)) return false;

    if (!make_queues()) return false;

    wr32(REG_AQA, ((QUEUE_ENTRIES - 1) << 16) | (QUEUE_ENTRIES - 1));
    wr64(REG_ASQ, (u64)admin.sq);
    wr64(REG_ACQ, (u64)admin.cq);

    /* Page size 4 KiB, the NVM command set, and the entry sizes the
       specification fixes: 64 bytes for a command, 16 for a completion,
       both written as powers of two. */
    u32 cc = (0u << 7)          /* MPS: 2^(12+0) = 4 KiB */
           | (0u << 4)          /* CSS: the NVM command set */
           | (6u << 16)         /* IOSQES: 2^6 = 64 */
           | (4u << 20)         /* IOCQES: 2^4 = 16 */
           | CC_ENABLE;
    wr32(REG_CC, cc);
    if (!wait_ready(true)) return false;

    /* What it is. The model number is a fixed field of spaces padded text
       rather than a terminated string. */
    static u8 ident[4096];
    if (!identify(1, 0, ident)) return false;
    memcpy(model, ident + 24, 40);
    model[40] = 0;
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--) model[i] = 0;

    if (!create_io_queues()) return false;

    /* Namespace one. A controller may present several; nothing here has a
       way to say which is wanted, and the first is what a disk has. */
    if (!identify(0, 1, ident)) return false;

    u64 nsze = *(u64 *)(ident + 0);            /* size, in logical blocks */
    u8  flbas = ident[26] & 0xF;               /* which format is in use */
    u32 lbaf = *(u32 *)(ident + 128 + flbas * 4);
    u32 lbads = (lbaf >> 16) & 0xFF;           /* as a power of two */

    if (lbads < 9 || lbads > 12) return false;
    sector_shift = lbads;

    /* The block layer counts in 512 byte sectors. A namespace formatted with
       4096 byte blocks is perfectly legal and is not something this can
       divide into, so it is refused rather than silently misread. */
    if (sector_shift != 9) return false;

    if (nsze == 0) return false;
    total_sectors = (nsze > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (u32)nsze;

    present = true;
    return true;
}

/* --- reading and writing --------------------------------------------------- */

static bool transfer(u8 opcode, u32 lba, u32 count, void *buf, bool write) {
    if (!present || count == 0 || count > MAX_SECTORS_PER_RUN) return false;
    if (lba > total_sectors || count > total_sectors - lba) return false;

    u32 bytes = count * SECTOR_SIZE;
    if (write) memcpy(buffer, buf, bytes);

    sqe_t c;
    memset(&c, 0, sizeof(c));
    c.opcode = opcode;
    c.nsid = 1;
    c.prp1 = (u64)buffer;
    c.cdw10 = lba;
    c.cdw11 = 0;                   /* the high half of the block address */
    c.cdw12 = count - 1;           /* a count of zero means one block */

    if (!submit(&io, &c)) return false;
    if (!write) memcpy(buf, buffer, bytes);
    return true;
}

bool nvme_read(u32 lba, u32 count, void *buf) {
    return transfer(IO_READ, lba, count, buf, false);
}

bool nvme_write(u32 lba, u32 count, const void *buf) {
    return transfer(IO_WRITE, lba, count, (void *)buf, true);
}

bool nvme_flush(void) {
    if (!present) return false;
    sqe_t c;
    memset(&c, 0, sizeof(c));
    c.opcode = IO_FLUSH;
    c.nsid = 1;
    return submit(&io, &c);
}
