/* SCSI, posted through two bulk endpoints.
 *
 * Every USB stick speaks this and almost nothing else: the interface says
 * class 8, subclass 6, protocol 0x50, and what that means is that SCSI
 * commands travel in a thirty one byte envelope, the data goes whichever way
 * the command asked for, and a thirteen byte receipt comes back. Three bulk
 * transfers per operation, in that order, always.
 *
 * The awkward parts are not the transfers. They are that a stick just
 * plugged in refuses the first command it is given on principle, that the
 * size of a sector is whatever the device says rather than 512, and that a
 * command which fails leaves a sense code the next command will trip over
 * unless somebody reads it. All three are handled below and all three are
 * the reason a driver that looks like it works on one device fails on the
 * next.
 */
#include "usbdisk.h"
#include "xhci.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "io.h"
#include "blackbox.h"
#include "blockdev.h"

#define CBW_SIG  0x43425355u          /* "USBC" */
#define CSW_SIG  0x53425355u          /* "USBS" */

/* What goes out in front of every command. */
typedef struct {
    u32 signature;
    u32 tag;                          /* comes back in the receipt */
    u32 length;                       /* bytes in the data stage */
    u8  flags;                        /* 0x80 to read, 0 to write */
    u8  lun;
    u8  cmd_len;
    u8  cmd[16];
} __attribute__((packed)) cbw_t;       /* thirty one bytes exactly */

/* And what comes back after it. */
typedef struct {
    u32 signature;
    u32 tag;
    u32 residue;                      /* what it did not manage to move */
    u8  status;                       /* 0 good, 1 failed, 2 confused */
} __attribute__((packed)) csw_t;       /* thirteen */

/* The SCSI commands this needs, which is fewer than it looks. */
#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY10  0x25
#define SCSI_READ10           0x28
#define SCSI_WRITE10          0x2A

#define SENSE_LEN   18
#define INQUIRY_LEN 36
#define STAGE_MAX   4096              /* the biggest reply this ever asks for */

static bool attached;
static u8   dev_slot, dev_in, dev_out;
static u32  nsectors;
static u32  sector_bytes = 512;
static char model[40];
static u32  next_tag = 1;

/* All three have to be identity mapped, because that is what the controller
   is given: it is told a physical address and writes there itself. Anything
   from the kernel heap is. */
static cbw_t *cbw;
static csw_t *csw;
static u8    *stage;

/* Waiting, without a timer.
 *
 * This runs during enumeration, which happens before interrupts are ever
 * switched on, and again from a task afterwards. A write to a port nobody
 * answers takes about a microsecond and works in both worlds; sleeping
 * works in only one of them. */
static void settle_ms(u32 ms) {
    while (ms--) for (u32 i = 0; i < 1000; i++) io_wait();
}

static u32 be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* One command, its data, and its receipt. */
static bool transact(const u8 *cmd, u8 cmd_len, void *data, u32 len, bool in) {
    if (!attached || cmd_len > 16) return false;

    memset(cbw, 0, sizeof *cbw);
    cbw->signature = CBW_SIG;
    cbw->tag = next_tag++;
    cbw->length = len;
    cbw->flags = in ? 0x80 : 0x00;
    cbw->lun = 0;
    cbw->cmd_len = cmd_len;
    memcpy(cbw->cmd, cmd, cmd_len);

    if (xhci_bulk(dev_slot, dev_out, cbw, sizeof *cbw, false)
        != (int)sizeof *cbw) return false;

    if (len && data) {
        int moved = xhci_bulk(dev_slot, in ? dev_in : dev_out, data, len, in);
        if (moved < 0) return false;
        /* Short is not a failure. A device is allowed to send less than was
           asked for and the receipt below says how much less. */
    }

    memset(csw, 0, sizeof *csw);
    if (xhci_bulk(dev_slot, dev_in, csw, sizeof *csw, true)
        != (int)sizeof *csw) return false;

    if (csw->signature != CSW_SIG) return false;
    if (csw->tag != cbw->tag) return false;
    return csw->status == 0;
}

/* Reads and throws away the sense data.
 *
 * Not for the information. A device that has failed a command keeps the
 * reason and refuses everything else until somebody asks for it, so this is
 * how the next command is allowed to run at all. */
static void clear_sense(void) {
    u8 cmd[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, SENSE_LEN, 0 };
    transact(cmd, sizeof cmd, stage, SENSE_LEN, true);
}

static bool unit_ready(void) {
    u8 cmd[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return transact(cmd, sizeof cmd, 0, 0, false);
}

/* A stick that has just been plugged in answers the first thing it is asked
   with a refusal, every time, by design: it is reporting that it has only
   just arrived. The way through is to ask, read the reason, and ask again,
   which is what every operating system does here and why a driver that tries
   once decides there is no disk. */
static bool wait_ready(void) {
    for (int i = 0; i < 20; i++) {
        if (unit_ready()) return true;
        clear_sense();
        settle_ms(50);
    }
    return false;
}

static void trim_into(char *out, u32 cap, const u8 *src, u32 n) {
    u32 w = 0;
    for (u32 i = 0; i < n && w + 1 < cap; i++) {
        char c = (char)src[i];
        if (c < 0x20 || c > 0x7E) c = ' ';
        out[w++] = c;
    }
    while (w && out[w - 1] == ' ') w--;       /* the padding is spaces */
    out[w] = 0;
}

static bool inquiry(void) {
    u8 cmd[6] = { SCSI_INQUIRY, 0, 0, 0, INQUIRY_LEN, 0 };
    memset(stage, 0, INQUIRY_LEN);
    if (!transact(cmd, sizeof cmd, stage, INQUIRY_LEN, true)) return false;

    /* Eight bytes of vendor then sixteen of product, both space padded. */
    char vendor[12];
    char product[20];
    trim_into(vendor, sizeof vendor, stage + 8, 8);
    trim_into(product, sizeof product, stage + 16, 16);

    u32 w = 0;
    for (u32 i = 0; vendor[i] && w + 1 < sizeof model; i++) model[w++] = vendor[i];
    if (w && w + 1 < sizeof model) model[w++] = ' ';
    for (u32 i = 0; product[i] && w + 1 < sizeof model; i++) model[w++] = product[i];
    model[w] = 0;
    if (!w) memcpy(model, "usb disk", 9);
    return true;
}

static bool read_capacity(void) {
    u8 cmd[10] = { SCSI_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    memset(stage, 0, 8);
    if (!transact(cmd, sizeof cmd, stage, 8, true)) return false;

    /* The address of the last sector, not how many there are, and the size
       of one. Both big endian, because SCSI predates the argument. */
    u32 last = be32(stage);
    u32 size = be32(stage + 4);
    if (!size || size > STAGE_MAX) return false;
    if (last == 0xFFFFFFFFu) return false;    /* needs the 16 byte version */

    nsectors = last + 1;
    sector_bytes = size;
    return true;
}

/* One read or write of up to sixty four kilobytes.
 *
 * The count is sixteen bits in the command, and the transfer has to fit in
 * whatever the caller's buffer is, so the splitting is the caller's job and
 * this refuses anything it cannot post in one go. */
static bool rw10(u32 lba, u32 count, void *buf, bool write) {
    if (!attached || !count || count > 0xFFFF) return false;
    u8 cmd[10];
    memset(cmd, 0, sizeof cmd);
    cmd[0] = write ? SCSI_WRITE10 : SCSI_READ10;
    cmd[2] = (u8)(lba >> 24);
    cmd[3] = (u8)(lba >> 16);
    cmd[4] = (u8)(lba >> 8);
    cmd[5] = (u8)lba;
    cmd[7] = (u8)(count >> 8);
    cmd[8] = (u8)count;

    if (!transact(cmd, sizeof cmd, buf, count * sector_bytes, !write)) {
        clear_sense();
        return false;
    }
    return true;
}

/* --- what the rest of the kernel sees ------------------------------------ */

bool usbdisk_present(void)   { return attached && nsectors != 0; }
u32  usbdisk_sectors(void)   { return nsectors; }
u32  usbdisk_block_size(void){ return sector_bytes; }
const char *usbdisk_model(void) { return model[0] ? model : "usb disk"; }

bool usbdisk_read(u32 lba, u32 count, void *buf) {
    if (!usbdisk_present() || !buf) return false;
    if (lba + count > nsectors) return false;
    return rw10(lba, count, buf, false);
}

bool usbdisk_write(u32 lba, u32 count, const void *buf) {
    if (!usbdisk_present() || !buf) return false;
    if (lba + count > nsectors) return false;
    return rw10(lba, count, (void *)buf, true);
}

/* Eight sectors at a time. A bulk transfer can be longer than that, but a
   stick is slow enough that the difference does not show and short requests
   keep the buffer a command needs small. */
static u32 usbdisk_max_run(void) { return 8; }

/* Nothing is held back, so there is nothing to push out. */
static bool usbdisk_flush(void) { return attached; }

static const blkdev_t USB_DEV = {
    "usb", usbdisk_read, usbdisk_write, usbdisk_flush,
    usbdisk_sectors, usbdisk_max_run, usbdisk_model, true
};

static u32 blk_id = BLK_NONE;

u32 usbdisk_blk_id(void) { return blk_id; }

bool usbdisk_attach(u8 slot, u8 in_dci, u8 out_dci) {
    if (attached) return false;               /* one stick is enough for now */

    if (!cbw)   cbw = (cbw_t *)kmalloc(sizeof *cbw);
    if (!csw)   csw = (csw_t *)kmalloc(sizeof *csw);
    if (!stage) stage = (u8 *)kmalloc(STAGE_MAX);
    if (!cbw || !csw || !stage) return false;

    attached = true;
    dev_slot = slot;
    dev_in = in_dci;
    dev_out = out_dci;
    nsectors = 0;
    sector_bytes = 512;
    model[0] = 0;

    if (!wait_ready()) { attached = false; return false; }
    inquiry();                                /* a name is not essential */
    if (!read_capacity()) { attached = false; return false; }

    /* Offered to the rest of the kernel as a disk, which is what makes it
       mountable rather than merely readable. */
    blk_id = blk_register(&USB_DEV);

    bb_log("usb disk %s, %d sectors of %d bytes, disk %d",
           usbdisk_model(), nsectors, sector_bytes, blk_id);
    return true;
}

void usbdisk_detach(u8 slot) {
    if (!attached || slot != dev_slot) return;
    if (blk_id != BLK_NONE) {
        blk_unregister(blk_id);
        blk_id = BLK_NONE;
    }
    attached = false;
    nsectors = 0;
    model[0] = 0;
    bb_log("usb disk unplugged");
}
