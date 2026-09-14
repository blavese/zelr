/* ATA PIO driver for the primary bus, master drive.
 *
 * PIO is the slow way to talk to a disk: the CPU moves every word itself
 * rather than handing the job to a DMA engine. It is also the way that needs
 * no bus mastering setup and works on essentially anything, which is the
 * right trade for a kernel this size. */
#include "ata.h"
#include "io.h"
#include "printf.h"
#include "string.h"

#define DATA      0x1F0
#define ERROR     0x1F1
#define SECCOUNT  0x1F2
#define LBA_LO    0x1F3
#define LBA_MID   0x1F4
#define LBA_HI    0x1F5
#define DRIVE     0x1F6
#define STATUS    0x1F7
#define COMMAND   0x1F7
#define CONTROL   0x3F6

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_SRV  0x10
#define ST_DF   0x20
#define ST_RDY  0x40
#define ST_BSY  0x80

#define CMD_READ    0x20
#define CMD_WRITE   0x30
#define CMD_FLUSH   0xE7
#define CMD_IDENTIFY 0xEC

static bool present = false;
static u32  total_sectors = 0;
static char model[41];

/* The status port needs ~400ns to settle after a command; four reads of the
   alternate status register is the conventional way to spend that time. */
static void delay400(void) {
    for (int i = 0; i < 4; i++) (void)inb(CONTROL);
}

static bool wait_not_busy(void) {
    for (u32 i = 0; i < 100000000u; i++) {
        u8 s = inb(STATUS);
        if (!(s & ST_BSY)) return true;
    }
    return false;
}

static bool wait_drq(void) {
    for (u32 i = 0; i < 100000000u; i++) {
        u8 s = inb(STATUS);
        if (s & ST_ERR) return false;
        if (!(s & ST_BSY) && (s & ST_DRQ)) return true;
    }
    return false;
}

bool ata_init(void) {
    present = false;
    total_sectors = 0;
    memset(model, 0, sizeof(model));

    outb(CONTROL, 0x02);            /* interrupts off, we poll */
    outb(DRIVE, 0xA0);              /* master */
    delay400();

    /* A floating bus reads back 0xFF: nothing is plugged in. */
    if (inb(STATUS) == 0xFF) return false;

    outb(SECCOUNT, 0);
    outb(LBA_LO, 0);
    outb(LBA_MID, 0);
    outb(LBA_HI, 0);
    outb(COMMAND, CMD_IDENTIFY);
    delay400();

    if (inb(STATUS) == 0) return false;        /* no drive here */
    if (!wait_not_busy()) return false;

    /* A non-zero LBA mid/hi after IDENTIFY means it is ATAPI, not a disk. */
    if (inb(LBA_MID) != 0 || inb(LBA_HI) != 0) return false;
    if (!wait_drq()) return false;

    u16 id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(DATA);

    /* Words 60-61 hold the LBA28 sector count. */
    total_sectors = ((u32)id[61] << 16) | id[60];

    /* Words 27-46 are the model string, with each pair byte swapped. */
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--) model[i] = 0;

    present = total_sectors > 0;
    return present;
}

bool ata_present(void)  { return present; }
u32  ata_sectors(void)  { return total_sectors; }

/* The sector count register is eight bits. Zero there means 256, which
   this does not use, so 255 is the most one command can move. */
u32  ata_max_run(void)  { return 255; }
const char *ata_model(void) { return model; }

static void select_lba(u32 lba, u8 count) {
    outb(DRIVE, (u8)(0xE0 | ((lba >> 24) & 0x0F)));   /* master, LBA mode */
    outb(SECCOUNT, count);
    outb(LBA_LO,  (u8)(lba & 0xFF));
    outb(LBA_MID, (u8)((lba >> 8) & 0xFF));
    outb(LBA_HI,  (u8)((lba >> 16) & 0xFF));
}

bool ata_read(u32 lba, u32 count, void *buf) {
    if (!present || count == 0 || count > 255) return false;
    if (lba + count > total_sectors) return false;

    u16 *out = (u16 *)buf;
    if (!wait_not_busy()) return false;
    select_lba(lba, (u8)count);
    outb(COMMAND, CMD_READ);

    for (u32 s = 0; s < count; s++) {
        if (!wait_drq()) return false;
        for (int i = 0; i < SECTOR_SIZE / 2; i++) *out++ = inw(DATA);
        delay400();
    }
    return true;
}

bool ata_write(u32 lba, u32 count, const void *buf) {
    if (!present || count == 0 || count > 255) return false;
    if (lba + count > total_sectors) return false;

    const u16 *in = (const u16 *)buf;
    if (!wait_not_busy()) return false;
    select_lba(lba, (u8)count);
    outb(COMMAND, CMD_WRITE);

    for (u32 s = 0; s < count; s++) {
        if (!wait_drq()) return false;
        /* No rep outsw here: some controllers need a moment between words. */
        for (int i = 0; i < SECTOR_SIZE / 2; i++) outw(DATA, *in++);
        delay400();
    }
    return ata_flush();
}

bool ata_flush(void) {
    if (!present) return false;
    if (!wait_not_busy()) return false;
    outb(DRIVE, 0xE0);
    outb(COMMAND, CMD_FLUSH);
    delay400();
    return wait_not_busy();
}
