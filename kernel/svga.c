/* VMware SVGA II.
 *
 * Two I/O ports reach every register: write the register number to the
 * index port, then read or write the value port. The registers say what
 * modes are possible, take the one wanted, and name the two pieces of
 * memory that matter: the aperture the pixels live in, and a command queue
 * called the FIFO.
 *
 * The FIFO is the part that is not like a plain framebuffer. On the cards
 * this kernel already drives, memory written to the aperture is on the
 * screen and there is nothing else to do. Here the adapter reads the
 * aperture only when it is told which rectangle changed, so a frame that is
 * copied and not announced leaves the screen exactly as it was. That is a
 * blank desktop rather than an error, which is worth knowing before
 * debugging it.
 */
#include "svga.h"
#include "pci.h"
#include "io.h"
#include "paging.h"
#include "printf.h"

#define SVGA_VENDOR 0x15AD
#define SVGA_DEVICE 0x0405

#define SVGA_INDEX_PORT 0
#define SVGA_VALUE_PORT 1

#define SVGA_REG_ID             0
#define SVGA_REG_ENABLE         1
#define SVGA_REG_WIDTH          2
#define SVGA_REG_HEIGHT         3
#define SVGA_REG_MAX_WIDTH      4
#define SVGA_REG_MAX_HEIGHT     5
#define SVGA_REG_BITS_PER_PIXEL 7
#define SVGA_REG_BYTES_PER_LINE 12
#define SVGA_REG_FB_START       13
#define SVGA_REG_FB_OFFSET      14
#define SVGA_REG_VRAM_SIZE      15
#define SVGA_REG_FB_SIZE        16
#define SVGA_REG_CAPABILITIES   17
#define SVGA_REG_MEM_START      18
#define SVGA_REG_MEM_SIZE       19
#define SVGA_REG_CONFIG_DONE    20
#define SVGA_REG_SYNC           21
#define SVGA_REG_BUSY           22

/* The id is a magic number and a version. The adapter is asked for the
   newest it might support and answers with the newest it does. */
#define SVGA_MAGIC  0x900000u
#define SVGA_ID(v)  ((SVGA_MAGIC << 8) | (v))

#define SVGA_CMD_UPDATE 1

/* The first four words of the FIFO are its own bookkeeping, not commands. */
#define FIFO_MIN       0
#define FIFO_MAX       1
#define FIFO_NEXT_CMD  2
#define FIFO_STOP      3
#define FIFO_HEADER    16u      /* those four words, in bytes */

static bool     present = false;
static u16      io_base;
static volatile u32 *fifo;
static u32      fifo_bytes;
static u32      mode_w, mode_h;

static void reg_write(u32 reg, u32 value) {
    outl((u16)(io_base + SVGA_INDEX_PORT), reg);
    outl((u16)(io_base + SVGA_VALUE_PORT), value);
}

static u32 reg_read(u32 reg) {
    outl((u16)(io_base + SVGA_INDEX_PORT), reg);
    return inl((u16)(io_base + SVGA_VALUE_PORT));
}

bool svga_present(void) { return present; }

/* Waits for the adapter to finish what it has been given. Also the only way
   to make room in a full queue. */
static void sync_fifo(void) {
    reg_write(SVGA_REG_SYNC, 1);
    while (reg_read(SVGA_REG_BUSY))
        ;
}

static void fifo_put(u32 value) {
    u32 min  = fifo[FIFO_MIN];
    u32 max  = fifo[FIFO_MAX];
    u32 next = fifo[FIFO_NEXT_CMD];

    /* Where the next word would land, wrapping at the end. Leaving it equal
       to STOP is how the queue says empty, so a write that would do that is
       a full queue and has to wait rather than look like nothing to do. */
    u32 after = next + 4;
    if (after >= max) after = min;

    if (after == fifo[FIFO_STOP]) {
        sync_fifo();
        /* After a sync the adapter has consumed everything, so there is
           room; re-read rather than assume where it left the cursor. */
        next  = fifo[FIFO_NEXT_CMD];
        after = next + 4;
        if (after >= max) after = min;
    }

    fifo[next / 4] = value;
    fifo[FIFO_NEXT_CMD] = after;
}

void svga_update(u32 x, u32 y, u32 w, u32 h) {
    if (!present || !w || !h) return;
    if (x >= mode_w || y >= mode_h) return;
    if (x + w > mode_w) w = mode_w - x;
    if (y + h > mode_h) h = mode_h - y;

    fifo_put(SVGA_CMD_UPDATE);
    fifo_put(x);
    fifo_put(y);
    fifo_put(w);
    fifo_put(h);
    sync_fifo();
}

bool svga_init(u32 width, u32 height, svga_mode_t *out) {
    present = false;

    pci_dev_t dev;
    if (!pci_find(SVGA_VENDOR, SVGA_DEVICE, &dev)) return false;

    /* The first BAR is a set of I/O ports rather than memory, so the low
       two bits are flags and not part of the address. */
    io_base = (u16)(dev.bar0 & ~0x3u);
    if (!io_base) return false;

    pci_enable_bus_master(&dev);

    /* Ask for the newest version this knows about and accept what comes
       back. Version 2 is what has the FIFO registers used below; older ones
       are not worth a second path here, and refusing is better than driving
       one as if it were the other. */
    reg_write(SVGA_REG_ID, SVGA_ID(2));
    if (reg_read(SVGA_REG_ID) != SVGA_ID(2)) return false;

    u32 max_w = reg_read(SVGA_REG_MAX_WIDTH);
    u32 max_h = reg_read(SVGA_REG_MAX_HEIGHT);
    if (width > max_w || height > max_h) return false;

    reg_write(SVGA_REG_ENABLE, 0);
    reg_write(SVGA_REG_WIDTH, width);
    reg_write(SVGA_REG_HEIGHT, height);
    reg_write(SVGA_REG_BITS_PER_PIXEL, 32);

    u64 fb_phys   = reg_read(SVGA_REG_FB_START);
    u32 fb_off    = reg_read(SVGA_REG_FB_OFFSET);
    u32 fb_size   = reg_read(SVGA_REG_FB_SIZE);
    u32 vram      = reg_read(SVGA_REG_VRAM_SIZE);
    u64 fifo_phys = reg_read(SVGA_REG_MEM_START);
    fifo_bytes    = reg_read(SVGA_REG_MEM_SIZE);
    u32 pitch     = reg_read(SVGA_REG_BYTES_PER_LINE);

    if (!fb_phys || !fifo_phys || fifo_bytes < FIFO_HEADER + 16) return false;
    if (pitch < width * 4) return false;

    /* The device answers with what it can do, which need not be what was
       asked for. Taking its word for the mode and then drawing at the
       requested one is how a screen ends up sheared. */
    if (reg_read(SVGA_REG_WIDTH) != width ||
        reg_read(SVGA_REG_HEIGHT) != height ||
        reg_read(SVGA_REG_BITS_PER_PIXEL) != 32) {
        reg_write(SVGA_REG_ENABLE, 0);
        return false;
    }

    u64 need = (u64)pitch * height;
    if (fb_size && need > fb_size) return false;
    if (vram && need > vram) return false;

    /* Both sit far above anything the kernel identity maps, so they need
       entries of their own. paging_map_device is what the other aperture
       here already goes through. */
    if (!paging_map_device(fb_phys + fb_off, need)) return false;
    void *f = paging_map_device(fifo_phys, fifo_bytes);
    if (!f) return false;

    fifo = (volatile u32 *)f;

    /* Hand the adapter an empty queue: the first word past the header is
       both where the next command goes and where it should stop reading. */
    fifo[FIFO_MIN]      = FIFO_HEADER;
    fifo[FIFO_MAX]      = fifo_bytes;
    fifo[FIFO_NEXT_CMD] = FIFO_HEADER;
    fifo[FIFO_STOP]     = FIFO_HEADER;
    reg_write(SVGA_REG_CONFIG_DONE, 1);

    reg_write(SVGA_REG_ENABLE, 1);

    mode_w = width;
    mode_h = height;
    present = true;

    out->fb_phys  = fb_phys + fb_off;
    out->fb_bytes = (u32)need;
    out->pitch    = pitch;
    out->width    = width;
    out->height   = height;
    return true;
}
