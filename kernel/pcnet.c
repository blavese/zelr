/* AMD Am79C970A, the PCnet-PCI II.
 *
 * This is the card VMware gives a machine whose guest type it does not
 * recognise, which is every machine running something written from scratch.
 * The e1000 is what VMware offers when it knows what it is talking to, and
 * the RTL8139 is QEMU's older default, so without this one the most likely
 * way anybody runs zelr on their own laptop has no network at all.
 *
 * It is the oldest design of the three and it shows. There are no registers
 * at fixed addresses: there is one address port and one data port, and every
 * register is reached by writing its number to the first and then reading or
 * writing the second. There are two such data ports, one for the control and
 * status registers and one for the bus configuration registers, and they are
 * different registers with the same numbers.
 *
 * The rings are not set up by writing their addresses to the card either.
 * Everything the card needs to know is written into a block of memory in a
 * fixed layout, the address of that block is given to the card, and the card
 * is told to go and read it. That layout is itself selectable, and the one
 * this driver uses is style 2, which is the 32-bit one.
 *
 * Lengths in descriptors are stored negated. A buffer of 2048 bytes is
 * written as the bottom twelve bits of -2048, and the four bits above that
 * have to be ones. Getting it wrong does not fail loudly: the card reads a
 * length of nearly nothing and every frame arrives truncated.
 *
 * The rings, the buffers and the initialisation block all come from the
 * kernel heap, which is identity mapped, so the address the driver sees is
 * the address the card needs. The card's addresses are 32 bits wide with no
 * upper half anywhere, so none of it may sit above 4 GiB. */
#include "pcnet.h"
#include "pci.h"
#include "idt.h"
#include "pic.h"
#include "heap.h"
#include "string.h"
#include "io.h"
#include "net.h"

#define VENDOR_AMD 0x1022
#define DEVICE_PCNET 0x2000

/* The first sixteen bytes of the I/O window are the address PROM, and the
   ports proper start after it. These are the 16-bit offsets; the card comes
   out of reset expecting them. */
#define APROM   0x00
#define REG_RDP 0x10        /* control and status register data */
#define REG_RAP 0x12        /* which register the data ports refer to */
#define REG_RESET 0x14      /* reading this resets the card */
#define REG_BDP 0x16        /* bus configuration register data */

/* CSR0, the one register worth naming the bits of. The interrupt flags are
   cleared by writing them back, so the same word both reports and
   acknowledges, and a careless read-modify-write of it clears interrupts
   that have not been looked at yet. */
#define CSR0_INIT  0x0001
#define CSR0_STRT  0x0002
#define CSR0_STOP  0x0004
#define CSR0_TDMD  0x0008
#define CSR0_INEA  0x0040
#define CSR0_IDON  0x0100
#define CSR0_TINT  0x0200
#define CSR0_RINT  0x0400
#define CSR0_ACK   0x7F00   /* every write-one-to-clear flag in the register */

/* Descriptor status, in the high half of the second word. */
#define OWN  0x8000         /* set means the card has it, not us */
#define ERR  0x4000
#define STP  0x0200         /* first descriptor of a frame */
#define ENP  0x0100         /* last descriptor of a frame */

/* The receive ring has to be able to hold a whole advertised window, or a
   peer that fills the window it was given overruns it every time and the
   loss is the driver's own doing. Sixty four buffers of two kilobytes is a
   hundred and twenty eight, against the sixty four this stack advertises. */
#define RX_LOG 6
#define TX_LOG 4
#define RX_DESCS (1u << RX_LOG)
#define TX_DESCS (1u << TX_LOG)
#define BUF_SIZE 2048

typedef struct {
    u32 base;
    u16 buf_length;         /* negated, top four bits ones */
    u16 status;
    u32 msg_length;         /* bottom twelve bits: bytes received, CRC included */
    u32 reserved;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    u32 base;
    u16 length;             /* negated, top four bits ones */
    u16 status;
    u32 misc;
    u32 reserved;
} __attribute__((packed)) tx_desc_t;

/* What the card reads to find out about itself. The two length fields are
   packed into one word, as the count of ring entries expressed as a power of
   two rather than as a number of descriptors. */
typedef struct {
    u16 mode;
    u16 tlen_rlen;
    u8  padr[6];
    u16 reserved;
    u8  ladr[8];
    u32 rdra;
    u32 tdra;
} __attribute__((packed)) init_block_t;

static pci_dev_t dev;
static u16 io_base;

/* Volatile, and not as a precaution. These are written by the card behind
   the compiler's back, so a loop that waits on an ownership bit is a loop
   whose condition the compiler is entitled to work out once, and handing a
   descriptor back is three stores whose order is the whole protocol: the
   card may take it the instant the ownership bit goes down, so the length
   beside it has to already be there. Built without this at -O2 the card
   works until there is enough traffic to keep it busy, and then quietly
   stops keeping up. */
static volatile rx_desc_t *rx_ring;
static volatile tx_desc_t *tx_ring;
static volatile init_block_t *init_block;
static u8 *rx_buf[RX_DESCS];
static u8 *tx_buf[TX_DESCS];
static u32 rx_cur, tx_cur;
static u8 mac[6];
static bool up;
static u32 rx_count, tx_count;

bool pcnet_up(void) { return up; }
const u8 *pcnet_mac(void) { return mac; }
u32 pcnet_rx_count(void) { return rx_count; }
u32 pcnet_tx_count(void) { return tx_count; }

static u16 csr_read(u16 reg) {
    outw(io_base + REG_RAP, reg);
    return inw(io_base + REG_RDP);
}
static void csr_write(u16 reg, u16 value) {
    outw(io_base + REG_RAP, reg);
    outw(io_base + REG_RDP, value);
}
static u16 bcr_read(u16 reg) {
    outw(io_base + REG_RAP, reg);
    return inw(io_base + REG_BDP);
}
static void bcr_write(u16 reg, u16 value) {
    outw(io_base + REG_RAP, reg);
    outw(io_base + REG_BDP, value);
}

/* A length as the card wants it: the bottom twelve bits of the negative,
   with the four above them set. */
static u16 neg_len(u16 len) { return (u16)((0u - (u32)len) & 0x0FFFu) | 0xF000u; }

/* Aligns an allocation upward; the rings want a 16 byte boundary and the
   initialisation block a 4 byte one. */
static void *alloc_aligned(u64 bytes, u64 align) {
    u8 *raw = (u8 *)kmalloc(bytes + align);
    if (!raw) return 0;
    u64 addr = ((u64)raw + align - 1) & ~(align - 1);
    if ((addr + bytes) >> 32) return 0;     /* the card cannot reach it */
    memset((void *)addr, 0, bytes);
    return (void *)addr;
}

static void handle_rx(void) {
    /* Walk forward while the card has given descriptors back. A frame can in
       principle span several of them, but every buffer here is larger than
       the biggest frame ethernet carries, so one descriptor is one frame and
       anything else is a fault. */
    while (!(rx_ring[rx_cur].status & OWN)) {
        u16 status = rx_ring[rx_cur].status;
        u32 len = rx_ring[rx_cur].msg_length & 0x0FFF;

        bool whole = (status & STP) && (status & ENP);
        if (whole && !(status & ERR) && len > 4 && len <= BUF_SIZE) {
            rx_count++;
            net_receive(rx_buf[rx_cur], (u16)(len - 4));   /* drop the CRC */
        }

        /* Hand the buffer back however it went, or the ring runs dry. The
           ownership bit goes last: it is what tells the card the rest of
           the descriptor is ready to be believed. */
        rx_ring[rx_cur].buf_length = neg_len(BUF_SIZE);
        rx_ring[rx_cur].msg_length = 0;
        rx_ring[rx_cur].status = OWN;

        rx_cur = (rx_cur + 1) % RX_DESCS;
    }
}

static void pcnet_isr(registers_t *r) {
    (void)r;
    u16 csr0 = csr_read(0);

    /* Acknowledging is writing the flags back, and the interrupt enable is
       in the same register and is not one of them: it is an ordinary bit
       that means what was last written to it. So a write that acknowledges
       and does not also assert it turns interrupts off, and the card is
       never heard from again. Everything after the first frame then arrives
       only when something happens to poll, which on a large transfer is not
       often enough and the ring overruns. */
    csr_write(0, (u16)((csr0 & CSR0_ACK) | CSR0_INEA));
    if (csr0 & CSR0_RINT) handle_rx();
}

bool pcnet_send(const void *data, u16 len) {
    if (!up || len == 0 || len > BUF_SIZE) return false;

    /* Ethernet will not carry a frame shorter than 60 bytes. The card can be
       told to pad for us, but then the length in the descriptor and the
       length on the wire disagree, so pad here and keep the two the same. */
    u16 total = len < 60 ? 60 : len;

    u32 i = tx_cur;
    if (tx_ring[i].status & OWN) return false;      /* ring full */

    memcpy(tx_buf[i], data, len);
    if (total > len) memset(tx_buf[i] + len, 0, (u32)(total - len));

    tx_ring[i].base = (u32)(u64)tx_buf[i];
    tx_ring[i].length = neg_len(total);
    tx_ring[i].misc = 0;
    tx_ring[i].status = OWN | STP | ENP;

    tx_cur = (tx_cur + 1) % TX_DESCS;
    csr_write(0, CSR0_INEA | CSR0_TDMD);            /* go and look now */

    for (u32 spin = 0; spin < 5000000u; spin++)
        if (!(tx_ring[i].status & OWN)) break;      /* the card took it */

    tx_count++;
    return true;
}

void pcnet_poll(void) {
    if (!up) return;
    /* handle_rx walks the ring and gives descriptors back, and the card's
       own interrupt does the same thing, so letting one interrupt the other
       loses frames. The walk is short. */
    bool were_on = interrupts_enabled();
    cli();
    handle_rx();
    if (were_on) sti();
}

static bool rings_init(void) {
    rx_ring = (volatile rx_desc_t *)alloc_aligned(sizeof(rx_desc_t) * RX_DESCS, 16);
    tx_ring = (volatile tx_desc_t *)alloc_aligned(sizeof(tx_desc_t) * TX_DESCS, 16);
    if (!rx_ring || !tx_ring) return false;

    for (u32 i = 0; i < RX_DESCS; i++) {
        rx_buf[i] = (u8 *)alloc_aligned(BUF_SIZE, 16);
        if (!rx_buf[i]) return false;
        rx_ring[i].base = (u32)(u64)rx_buf[i];
        rx_ring[i].buf_length = neg_len(BUF_SIZE);
        rx_ring[i].msg_length = 0;
        rx_ring[i].status = OWN;            /* the card owns them all to start */
    }
    for (u32 i = 0; i < TX_DESCS; i++) {
        tx_buf[i] = (u8 *)alloc_aligned(BUF_SIZE, 16);
        if (!tx_buf[i]) return false;
        tx_ring[i].base = (u32)(u64)tx_buf[i];
        tx_ring[i].length = 0;
        tx_ring[i].misc = 0;
        tx_ring[i].status = 0;              /* and none of these */
    }
    rx_cur = tx_cur = 0;
    return true;
}

bool pcnet_init(void) {
    up = false;

    if (!pci_find(VENDOR_AMD, DEVICE_PCNET, &dev)) return false;

    pci_enable_bus_master(&dev);
    io_base = (u16)(dev.bar0 & ~0x3u);
    if (!io_base) return false;

    /* The address is in the PROM whatever state the card is in, and reading
       it first means a card that will not reset can still be named. */
    for (int i = 0; i < 6; i++) mac[i] = inb(io_base + APROM + i);

    /* Reading the reset register is the reset. It also puts the card back
       into the 16-bit port mode this driver uses, so it has to happen before
       anything else is touched. */
    (void)inw(io_base + REG_RESET);
    for (volatile u32 d = 0; d < 1000; d++) { }

    /* A card that is there and reset reads back stopped. Anything else is
       something at this address that is not one of these. */
    if (csr_read(0) != CSR0_STOP) return false;

    /* Style 2: 32-bit descriptors and a 32-bit initialisation block. The
       low byte alone selects it, and the rest of the register is other
       people's business. */
    bcr_write(20, (u16)((bcr_read(20) & ~0x00FFu) | 2u));

    /* Let the card work out for itself which of its interfaces has a cable
       in it. Under a hypervisor there is only one, but this driver is meant
       to reach a real machine too. */
    bcr_write(2, (u16)(bcr_read(2) | 0x0002u));

    /* The only thing worth an interrupt here is a frame having arrived.
       Everything else this card can interrupt about it either recovers from
       on its own or reports in a register nothing reads.

       This is not tidiness. An interrupt source that is left unmasked and is
       never cleared holds the line down for good, and the line is level
       triggered: the handler returns, the card asserts again, and the
       machine spends the rest of its life in the handler. Two of these fire
       in ordinary use. One is the start of every transmission. The other is
       the missed frame counter overflowing, which is what a receive ring
       that fell behind does, so the failure arrives exactly when there is
       enough traffic to matter and never on the small transfers that get
       tried first. */
    csr_write(3, 0x5B00);              /* mask all but a received frame */
    csr_write(4, (u16)(csr_read(4) | 0x0115u));   /* and the four in here */

    if (!rings_init()) return false;

    init_block = (volatile init_block_t *)alloc_aligned(sizeof(init_block_t), 16);
    if (!init_block) return false;

    init_block->mode = 0;               /* our address, and broadcast */
    init_block->tlen_rlen = (u16)((TX_LOG << 12) | (RX_LOG << 4));
    for (int i = 0; i < 6; i++) init_block->padr[i] = mac[i];
    init_block->reserved = 0;
    for (int i = 0; i < 8; i++) init_block->ladr[i] = 0;   /* no multicast */
    init_block->rdra = (u32)(u64)rx_ring;
    init_block->tdra = (u32)(u64)tx_ring;

    csr_write(1, (u16)((u64)init_block & 0xFFFF));
    csr_write(2, (u16)(((u64)init_block >> 16) & 0xFFFF));

    /* Now go and read it. The card says it has by raising IDON. */
    csr_write(0, CSR0_INIT);
    bool ready = false;
    for (u32 spin = 0; spin < 1000000u; spin++) {
        if (csr_read(0) & CSR0_IDON) { ready = true; break; }
    }
    if (!ready) return false;

    register_interrupt_handler(32 + dev.irq, pcnet_isr);
    pic_unmask(dev.irq);
    if (dev.irq >= 8) pic_unmask(2);

    /* Clear the initialisation flag, turn interrupts on and start. */
    csr_write(0, CSR0_IDON | CSR0_INEA | CSR0_STRT);

    up = true;
    return true;
}
