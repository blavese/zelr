/* Intel 82540EM (e1000).
 *
 * This is the card VirtualBox and VMware present by default, so supporting it
 * is what lets zelr run somewhere other than QEMU. Unlike the RTL8139 it is
 * driven through memory mapped registers and descriptor rings rather than
 * port I/O, and it does the DMA itself: the driver hands it a list of buffers
 * and moves a tail pointer.
 *
 * Descriptor rings and packet buffers come from the kernel heap, which lives
 * inside the identity mapped region, so the address the driver sees is the
 * address the card needs. */
#include "e1000.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "idt.h"
#include "pic.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "net.h"

#define VENDOR_INTEL 0x8086
static const u16 SUPPORTED[] = { 0x100E, 0x1015, 0x1004, 0x100F, 0x10D3, 0x153A };

#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200
#define REG_RAL     0x5400
#define REG_RAH     0x5404

#define CTRL_SLU    (1u << 6)      /* set link up */
#define CTRL_ASDE   (1u << 5)

#define RCTL_EN     (1u << 1)
#define RCTL_SBP    (1u << 2)
#define RCTL_UPE    (1u << 3)
#define RCTL_MPE    (1u << 4)
#define RCTL_BAM    (1u << 15)
#define RCTL_SECRC  (1u << 26)
#define RCTL_SZ_2048 0

#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)
#define TCTL_CT     (0x0F << 4)
#define TCTL_COLD   (0x40 << 12)

#define ICR_RXT0    (1u << 7)

#define RX_DESCS 32
#define TX_DESCS 16
#define BUF_SIZE 2048

typedef struct {
    u64 addr;
    u16 length;
    u16 checksum;
    u8  status;
    u8  errors;
    u16 special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    u64 addr;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} __attribute__((packed)) tx_desc_t;

static pci_dev_t dev;
static volatile u8 *mmio;

/* The card writes these, so a wait on a status bit is a wait on something
   the compiler cannot see change. Without this it is entitled to read the
   word once and spin on the answer. */
static volatile rx_desc_t *rx_ring;
static volatile tx_desc_t *tx_ring;
static u8 *rx_buf[RX_DESCS];
static u8 *tx_buf[TX_DESCS];
static u32 rx_cur, tx_cur;
static u8  mac[6];
static bool up;
static u32 rx_count, tx_count;

bool e1000_up(void) { return up; }
const u8 *e1000_mac(void) { return mac; }
u32 e1000_rx_count(void) { return rx_count; }
u32 e1000_tx_count(void) { return tx_count; }

static void wr(u32 reg, u32 value) { *(volatile u32 *)(mmio + reg) = value; }
static u32 rd(u32 reg) { return *(volatile u32 *)(mmio + reg); }

/* Aligns an allocation upward; the rings must sit on a 16 byte boundary. */
static void *alloc_aligned(u64 bytes, u64 align, void **raw_out) {
    u8 *raw = (u8 *)kmalloc(bytes + align);
    if (!raw) return 0;
    *raw_out = raw;
    u64 addr = ((u64)raw + align - 1) & ~(align - 1);
    memset((void *)addr, 0, bytes);
    return (void *)addr;
}

static bool read_mac(void) {
    /* The card latches its address into the receive address registers at
       reset, which is simpler and more reliable than walking the EEPROM. */
    u32 low = rd(REG_RAL);
    u32 high = rd(REG_RAH);

    if (low != 0 || (high & 0xFFFF) != 0) {
        mac[0] = (u8)(low);        mac[1] = (u8)(low >> 8);
        mac[2] = (u8)(low >> 16);  mac[3] = (u8)(low >> 24);
        mac[4] = (u8)(high);       mac[5] = (u8)(high >> 8);
        return true;
    }

    /* Fall back to the EEPROM for cards that do not pre-load it. */
    for (int i = 0; i < 3; i++) {
        wr(REG_EERD, ((u32)i << 8) | 1);
        u32 v = 0;
        for (u32 spin = 0; spin < 1000000; spin++) {
            v = rd(REG_EERD);
            if (v & (1u << 4)) break;
        }
        if (!(v & (1u << 4))) return false;
        u16 word = (u16)(v >> 16);
        mac[i * 2]     = (u8)(word & 0xFF);
        mac[i * 2 + 1] = (u8)(word >> 8);
    }
    return true;
}

static bool rx_init(void) {
    void *raw;
    rx_ring = (volatile rx_desc_t *)alloc_aligned(sizeof(rx_desc_t) * RX_DESCS, 16, &raw);
    if (!rx_ring) return false;

    for (int i = 0; i < RX_DESCS; i++) {
        rx_buf[i] = (u8 *)kmalloc(BUF_SIZE);
        if (!rx_buf[i]) return false;
        memset(rx_buf[i], 0, BUF_SIZE);
        rx_ring[i].addr = (u64)rx_buf[i];
        rx_ring[i].status = 0;
    }

    wr(REG_RDBAL, (u32)(u64)rx_ring);
    wr(REG_RDBAH, (u32)((u64)rx_ring >> 32));
    wr(REG_RDLEN, sizeof(rx_desc_t) * RX_DESCS);
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_DESCS - 1);        /* the card owns everything up to here */
    rx_cur = 0;

    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_SZ_2048 | RCTL_UPE | RCTL_MPE);
    return true;
}

static bool tx_init(void) {
    void *raw;
    tx_ring = (volatile tx_desc_t *)alloc_aligned(sizeof(tx_desc_t) * TX_DESCS, 16, &raw);
    if (!tx_ring) return false;

    for (int i = 0; i < TX_DESCS; i++) {
        tx_buf[i] = (u8 *)kmalloc(BUF_SIZE);
        if (!tx_buf[i]) return false;
        memset(tx_buf[i], 0, BUF_SIZE);
        tx_ring[i].addr = (u64)tx_buf[i];
        tx_ring[i].status = 1;        /* descriptor done: free to use */
        tx_ring[i].cmd = 0;
    }

    wr(REG_TDBAL, (u32)(u64)tx_ring);
    wr(REG_TDBAH, (u32)((u64)tx_ring >> 32));
    wr(REG_TDLEN, sizeof(tx_desc_t) * TX_DESCS);
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    tx_cur = 0;

    wr(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    wr(REG_TIPG, 0x0060200A);
    return true;
}

static void handle_rx(void) {
    while (rx_ring[rx_cur].status & 0x01) {          /* descriptor done */
        u16 len = rx_ring[rx_cur].length;
        if (len && len <= BUF_SIZE) {
            rx_count++;
            net_receive(rx_buf[rx_cur], len);
        }
        rx_ring[rx_cur].status = 0;

        u32 old = rx_cur;
        rx_cur = (rx_cur + 1) % RX_DESCS;
        wr(REG_RDT, old);                            /* hand the buffer back */
    }
}

static void e1000_isr(registers_t *r) {
    (void)r;
    u32 cause = rd(REG_ICR);                         /* reading clears it */
    if (cause & ICR_RXT0) handle_rx();
}

bool e1000_send(const void *data, u16 len) {
    if (!up || len == 0 || len > BUF_SIZE) return false;

    /* Ethernet will not carry a frame shorter than 60 bytes. Copy what the
       caller actually gave us and zero the rest: reading up to the padded
       length instead would run off the end of the caller's frame and put
       whatever followed it, which is the previous packet, on the wire. */
    u16 total = len < 60 ? 60 : len;

    u32 i = tx_cur;
    memcpy(tx_buf[i], data, len);
    if (total > len) memset(tx_buf[i] + len, 0, (u32)(total - len));
    tx_ring[i].length = total;
    tx_ring[i].cmd = (1 << 0) | (1 << 1) | (1 << 3);  /* EOP, IFCS, RS */
    tx_ring[i].status = 0;

    tx_cur = (tx_cur + 1) % TX_DESCS;
    wr(REG_TDT, tx_cur);

    for (u32 spin = 0; spin < 5000000; spin++)
        if (tx_ring[i].status & 0x0F) break;          /* the card reports back */

    tx_count++;
    return true;
}

void e1000_poll(void) {
    if (!up) return;
    /* handle_rx walks the ring and moves the tail pointer. The card's own
       interrupt does the same thing, so letting one interrupt the other
       corrupts the cursor and packets go missing. Shut interrupts out for
       the walk; it is short. */
    bool were_on = interrupts_enabled();
    cli();
    handle_rx();
    if (were_on) sti();
}

bool e1000_init(void) {
    up = false;

    bool found = false;
    for (u32 i = 0; i < sizeof(SUPPORTED) / sizeof(SUPPORTED[0]); i++) {
        if (pci_find(VENDOR_INTEL, SUPPORTED[i], &dev)) { found = true; break; }
    }
    if (!found) return false;

    pci_enable_bus_master(&dev);

    u64 phys = dev.bar0 & 0xFFFFFFF0u;
    if (!phys) return false;

    /* The register block sits well above the identity mapped region, so it
       needs mappings of its own before a single register can be touched. */
    mmio = (volatile u8 *)paging_map_device(phys, 0x20000);
    if (!mmio) return false;

    wr(REG_IMC, 0xFFFFFFFF);                          /* interrupts off while we set up */
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    for (u32 i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    if (!read_mac()) return false;

    if (!rx_init() || !tx_init()) return false;

    register_interrupt_handler(32 + dev.irq, e1000_isr);
    pic_unmask(dev.irq);
    if (dev.irq >= 8) pic_unmask(2);

    wr(REG_IMS, ICR_RXT0);
    (void)rd(REG_ICR);

    up = true;
    return true;
}
