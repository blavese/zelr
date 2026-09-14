/* AHCI (SATA).
 *
 * Modern machines and modern virtual machines present their disks through an
 * AHCI controller rather than the IDE interface the ATA PIO driver speaks, so
 * this is what lets nyx see a disk anywhere but QEMU's legacy default.
 *
 * The model is different from PIO in a useful way: instead of the CPU moving
 * every word through a port, the driver builds a command in memory, points
 * the controller at it, and the controller does the transfer itself. All the
 * structures below are laid out exactly as the specification requires, and
 * the alignment rules are not optional: the command list must be on a 1 KiB
 * boundary and the received-FIS area on a 256 byte one.
 */
#include "ahci.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

#define SATA_SIG_ATA   0x00000101

#define HBA_PORT_DET_PRESENT 3
#define HBA_PORT_IPM_ACTIVE  1

#define HBA_PxCMD_ST   0x0001
#define HBA_PxCMD_FRE  0x0010
#define HBA_PxCMD_FR   0x4000
#define HBA_PxCMD_CR   0x8000

#define ATA_CMD_READ_DMA_EX  0x25
#define ATA_CMD_WRITE_DMA_EX 0x35
#define ATA_CMD_IDENTIFY     0xEC

#define ATA_DEV_BUSY 0x80
#define ATA_DEV_DRQ  0x08

#define FIS_TYPE_REG_H2D 0x27

typedef volatile struct {
    u32 clb, clbu, fb, fbu;
    u32 is, ie, cmd, rsv0;
    u32 tfd, sig, ssts, sctl, serr, sact, ci, sntf;
    u32 fbs;
    u32 rsv1[11];
    u32 vendor[4];
} hba_port_t;

typedef volatile struct {
    u32 cap, ghc, is, pi, vs, ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
    u8  rsv[0xA0 - 0x2C];
    u8  vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} hba_mem_t;

typedef struct {
    u8  cfl_a_w_p;        /* command FIS length in low 5 bits, then A, W, P */
    u8  r_b_c_pmp;
    u16 prdtl;
    volatile u32 prdbc;
    u32 ctba, ctbau;
    u32 rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct {
    u32 dba, dbau, rsv0;
    u32 dbc_i;            /* byte count in low 22 bits, interrupt in bit 31 */
} __attribute__((packed)) hba_prdt_entry_t;

typedef struct {
    u8 cfis[64];
    u8 acmd[16];
    u8 rsv[48];
    hba_prdt_entry_t prdt[8];
} __attribute__((packed)) hba_cmd_tbl_t;

typedef struct {
    u8 fis_type;
    u8 pmport_c;          /* port multiplier in low 4 bits, C in bit 7 */
    u8 command, featurel;
    u8 lba0, lba1, lba2, device;
    u8 lba3, lba4, lba5, featureh;
    u8 countl, counth, icc, control;
    u8 rsv1[4];
} __attribute__((packed)) fis_reg_h2d_t;

static hba_mem_t  *hba;
static hba_port_t *port;
static int         port_no = -1;
static bool        present;
static u32         total_sectors;
static char        model[41];

static hba_cmd_header_t *cmd_list;
static hba_cmd_tbl_t    *cmd_tbl;
static u8               *fis_area;
static u8               *dma_buf;      /* bounce buffer for one transfer */

bool ahci_present(void) { return present; }
u32  ahci_sectors(void) { return total_sectors; }

/* One PRDT entry pointing at one staging buffer, and that buffer is four
   kilobytes. Eight sectors fill it exactly. */
u32  ahci_max_run(void) { return 8; }
const char *ahci_model(void) { return model; }

/* Allocates zeroed memory on a given alignment. The heap is inside the
   identity mapped region, so the address returned is also the physical one
   the controller needs. */
static void *alloc_aligned(u64 bytes, u64 align) {
    u8 *raw = (u8 *)kmalloc(bytes + align);
    if (!raw) return 0;
    u64 addr = ((u64)raw + align - 1) & ~(align - 1);
    memset((void *)addr, 0, bytes);
    return (void *)addr;
}

static void stop_port(void) {
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;
    for (u32 i = 0; i < 1000000; i++) {
        if (port->cmd & HBA_PxCMD_FR) continue;
        if (port->cmd & HBA_PxCMD_CR) continue;
        break;
    }
}

static void start_port(void) {
    while (port->cmd & HBA_PxCMD_CR) { }
    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

/* Returns the index of a free command slot, or -1. */
static int find_slot(void) {
    u32 busy = port->sact | port->ci;
    for (int i = 0; i < 32; i++)
        if (!(busy & (1u << i))) return i;
    return -1;
}

static bool wait_done(int slot) {
    for (u32 spin = 0; spin < 20000000u; spin++) {
        if (!(port->ci & (1u << slot))) return true;
        if (port->is & (1u << 30)) return false;      /* task file error */
    }
    return false;
}

/* Issues one command. `write` selects the direction, `count` is in sectors,
   and the data always moves through dma_buf. */
static bool run_command(u8 command, u64 lba, u16 count, bool write, u32 bytes) {
    port->is = (u32)-1;                                /* clear stale status */

    int slot = find_slot();
    if (slot < 0) return false;

    hba_cmd_header_t *hdr = &cmd_list[slot];
    hdr->cfl_a_w_p = (u8)((sizeof(fis_reg_h2d_t) / 4) & 0x1F);
    if (write) hdr->cfl_a_w_p |= (1 << 6);             /* W: host to device */
    hdr->prdtl = 1;
    hdr->prdbc = 0;
    /* The controller reads these as one 64-bit address, so the upper half
       has to be right even when it happens to be zero. */
    hdr->ctba = (u32)(u64)cmd_tbl;
    hdr->ctbau = (u32)((u64)cmd_tbl >> 32);

    memset(cmd_tbl, 0, sizeof(hba_cmd_tbl_t));
    cmd_tbl->prdt[0].dba = (u32)(u64)dma_buf;
    cmd_tbl->prdt[0].dbau = (u32)((u64)dma_buf >> 32);
    cmd_tbl->prdt[0].dbc_i = (bytes - 1) | (1u << 31); /* byte count is n-1 */

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 1 << 7;                            /* this is a command */
    fis->command = command;
    fis->lba0 = (u8)(lba);
    fis->lba1 = (u8)(lba >> 8);
    fis->lba2 = (u8)(lba >> 16);
    fis->device = 1 << 6;                              /* LBA mode */
    fis->lba3 = (u8)(lba >> 24);
    fis->lba4 = (u8)(lba >> 32);
    fis->lba5 = (u8)(lba >> 40);
    fis->countl = (u8)(count & 0xFF);
    fis->counth = (u8)(count >> 8);

    /* The drive must not be mid-transfer before a new command is issued. */
    for (u32 spin = 0; spin < 10000000u; spin++)
        if (!(port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ))) break;

    port->ci = 1u << slot;
    return wait_done(slot);
}

bool ahci_read(u32 lba, u32 count, void *buf) {
    if (!present || count == 0 || count > 8) return false;
    if (lba + count > total_sectors) return false;

    u32 bytes = count * 512;
    if (!run_command(ATA_CMD_READ_DMA_EX, lba, (u16)count, false, bytes)) return false;
    memcpy(buf, dma_buf, bytes);
    return true;
}

bool ahci_write(u32 lba, u32 count, const void *buf) {
    if (!present || count == 0 || count > 8) return false;
    if (lba + count > total_sectors) return false;

    u32 bytes = count * 512;
    memcpy(dma_buf, buf, bytes);
    return run_command(ATA_CMD_WRITE_DMA_EX, lba, (u16)count, true, bytes);
}

bool ahci_flush(void) { return present; }   /* DMA writes are already through */

static bool identify(void) {
    if (!run_command(ATA_CMD_IDENTIFY, 0, 1, false, 512)) return false;

    u16 *id = (u16 *)dma_buf;

    /* Words 100..103 hold the 48-bit count; 60..61 the 28-bit one. */
    u64 big = ((u64)id[103] << 48) | ((u64)id[102] << 32) |
              ((u64)id[101] << 16) | id[100];
    u32 small = ((u32)id[61] << 16) | id[60];
    u64 sectors = big ? big : small;
    if (sectors > 0xFFFFFFFFull) sectors = 0xFFFFFFFFull;
    total_sectors = (u32)sectors;

    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--) model[i] = 0;

    return total_sectors > 0;
}

bool ahci_init(void) {
    present = false;
    total_sectors = 0;
    memset(model, 0, sizeof(model));

    pci_dev_t dev;
    /* AHCI identifies itself by class rather than by a device id, so any
       vendor's controller is found the same way. */
    if (!pci_find_class(0x01, 0x06, 0x01, &dev)) return false;

    pci_enable_bus_master(&dev);

    u64 abar = pci_read32(dev.bus, dev.slot, dev.func, 0x24) & 0xFFFFFFF0u;
    if (!abar) return false;

    hba = (hba_mem_t *)paging_map_device(abar, 0x2000);
    if (!hba) return false;

    hba->ghc |= (1u << 31);                    /* AHCI enable */

    /* Take the first port with a SATA disk actually attached. */
    u32 pi = hba->pi;
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        hba_port_t *p = &hba->ports[i];
        u32 det = p->ssts & 0x0F;
        u32 ipm = (p->ssts >> 8) & 0x0F;
        if (det != HBA_PORT_DET_PRESENT || ipm != HBA_PORT_IPM_ACTIVE) continue;
        if (p->sig != SATA_SIG_ATA) continue;   /* skip ATAPI and port multipliers */
        port_no = i;
        port = p;
        break;
    }
    if (port_no < 0) return false;

    stop_port();

    cmd_list = (hba_cmd_header_t *)alloc_aligned(32 * sizeof(hba_cmd_header_t), 1024);
    fis_area = (u8 *)alloc_aligned(256, 256);
    cmd_tbl  = (hba_cmd_tbl_t *)alloc_aligned(sizeof(hba_cmd_tbl_t), 128);
    dma_buf  = (u8 *)alloc_aligned(8 * 512, 4096);
    if (!cmd_list || !fis_area || !cmd_tbl || !dma_buf) return false;

    port->clb = (u32)(u64)cmd_list;
    port->clbu = (u32)((u64)cmd_list >> 32);
    port->fb  = (u32)(u64)fis_area;
    port->fbu = (u32)((u64)fis_area >> 32);
    port->serr = (u32)-1;
    port->is = (u32)-1;
    port->ie = 0;                               /* polled, not interrupt driven */

    start_port();

    if (!identify()) { stop_port(); return false; }

    present = true;
    return true;
}
