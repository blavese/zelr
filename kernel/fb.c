/* Linear framebuffer.
 *
 * Mode setting goes through the Bochs VBE dispatch interface: a pair of I/O
 * ports that QEMU's std VGA, VirtualBox and Bochs all implement. It needs no
 * real mode, no BIOS call and no bootloader cooperation, which is what makes
 * it usable from a kernel that is already in protected mode.
 *
 * Drawing goes to a back buffer in RAM and is pushed to the card in one go.
 * Reading from video memory over PCI is slow enough that compositing directly
 * in it is visibly sluggish. */
#include "fb.h"
#include "io.h"
#include "svga.h"
#include "io.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "smp.h"

#define VBE_INDEX 0x01CE
#define VBE_DATA  0x01CF

#define VBE_ID       0
#define VBE_XRES     1
#define VBE_YRES     2
#define VBE_BPP      3
#define VBE_ENABLE   4
#define VBE_BANK     5
#define VBE_VWIDTH   6
#define VBE_VHEIGHT  7
#define VBE_XOFF     8
#define VBE_YOFF     9

#define VBE_DISABLED 0x00
#define VBE_ENABLED  0x01
#define VBE_LFB      0x40

/* QEMU / Bochs std VGA */
#define VGA_VENDOR 0x1234
#define VGA_DEVICE 0x1111
/* VirtualBox ships the same VBE interface under a different id */
#define VBOX_VENDOR 0x80EE
#define VBOX_DEVICE 0xBEEF

static bool   active = false;
/* The adapter on VMware needs to be told which pixels changed, and no
   other one here does, so the flush has to know which it is driving. */
static bool   via_svga = false;
static bool   adopted  = false;

/* How long it takes to put the whole back buffer on the screen, measured on
   the one this does at startup so it costs nothing to know. In cycles,
   because the timer does not exist yet when video is set up, and because
   what is wanted from it is a comparison rather than a duration. */
static u64    flush_cycles;
static u32    width, height, pitch;
static u8    *lfb;          /* mapped video memory */
static u8    *back;         /* back buffer we actually draw into */

/* A copy of what the card was last given, so that a frame can be compared
   against it and only the parts that differ sent. See fb_flush. Without room
   for it the flush is the whole screen every time, which is what it used to
   be, so a machine too small for this one still works. */
static u8    *sent;
static bool   sent_valid;

/* Drawing goes through a back buffer so that a half drawn frame is never on
   the screen. When there is not enough memory for one, drawing straight into
   video memory is still worth doing: it tears, and tearing is the difference
   between a screen and a machine that looks like it never booted. */
static bool take_back_buffer(u64 bytes) {
    back = (u8 *)kmalloc(bytes);
    if (back) return true;
    back = lfb;
    return false;
}

/* The mirror. Failing to get one is not a failure: it costs the comparison
   and nothing else, and the screen still works. */
static void take_sent(u64 bytes) {
    if (sent) { kfree(sent); sent = 0; }
    sent_valid = false;
    if (back == lfb) return;                 /* nothing to compare against */
    sent = (u8 *)kmalloc(bytes);
}

static void vbe_write(u16 reg, u16 value) {
    outw(VBE_INDEX, reg);
    outw(VBE_DATA, value);
}

static u16 vbe_read(u16 reg) {
    outw(VBE_INDEX, reg);
    return inw(VBE_DATA);
}

bool fb_active(void) { return active; }

const char *fb_backend(void) {
    if (!active)    return "none";
    if (adopted)    return "adopted from the loader";
    if (via_svga)   return "set through the vmware adapter";
    return "set through vbe";
}
u32  fb_width(void)  { return width; }
u32  fb_height(void) { return height; }
u32  fb_pitch(void)  { return pitch; }
u8  *fb_pixels(void) { return back; }

/* Adopts a screen the firmware already set up.
 *
 * On a UEFI machine there is no VBE and no way to change the mode after the
 * firmware is gone, so whatever the loader chose is what there is. The
 * aperture is wherever the firmware put it, which on a real machine is
 * usually a long way above where the kernel identity maps. */
bool fb_adopt(u64 base, u32 w, u32 h, u32 pitch_pixels) {
    active = false;
    via_svga = false;
    adopted = true;
    if (!base || !w || !h) return false;

    width = w;
    height = h;
    pitch = pitch_pixels * 4;

    u64 bytes = (u64)pitch * height;
    /* Write combining rather than uncached. The whole screen is copied
       through this aperture every time anything changes, and uncached made
       that copy most of what a frame cost. */
    if (!paging_map_wc(base, bytes)) return false;
    lfb = (u8 *)base;

    take_back_buffer(bytes);
    take_sent(bytes);

    active = true;
    fb_clear(0);
    fb_flush();
    return true;
}

/* VMware's adapter, tried when there is no VBE to ask.
 *
 * Kept apart from fb_init below rather than folded into it: the two set a
 * mode in entirely different ways, and the only thing they share is what
 * they leave behind. */
static bool init_svga(u32 w, u32 h) {
    svga_mode_t m;
    if (!svga_init(w, h, &m)) return false;

    width  = m.width;
    height = m.height;
    pitch  = m.pitch;
    lfb    = (u8 *)m.fb_phys;

    back = (u8 *)kmalloc((u64)pitch * height);
    if (!back) return false;
    take_sent((u64)pitch * height);

    via_svga = true;
    active = true;
    fb_clear(0);
    fb_flush();
    return true;
}

/* Is there a card here whose mode we can set at all. */
static bool bga_present(pci_dev_t *out) {
    u16 id = vbe_read(VBE_ID);       /* 0xB0C2 or later knows about the lfb */
    if (id < 0xB0C0 || id > 0xB0CF) return false;
    return pci_find(VGA_VENDOR, VGA_DEVICE, out)
        || pci_find(VBOX_VENDOR, VBOX_DEVICE, out);
}

/* Sets a mode and leaves width, height, pitch and lfb describing it.
 *
 * The back buffer is deliberately not this function's business. Changing
 * mode while the machine is running has to be able to put the previous one
 * back when the card refuses, and it cannot do that if the buffer it was
 * drawing into has already been thrown away. */
static bool bga_mode(u32 w, u32 h) {
    pci_dev_t vga;
    if (!bga_present(&vga)) return false;

    u64 phys = vga.bar0 & 0xFFFFFFF0u;
    if (!phys) return false;

    vbe_write(VBE_ENABLE, VBE_DISABLED);
    vbe_write(VBE_XRES, (u16)w);
    vbe_write(VBE_YRES, (u16)h);
    vbe_write(VBE_BPP, 32);
    vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB);

    /* Confirm the card actually took the mode rather than assuming. A card
       with too little memory for the size asked for says so here, which is
       the whole of what makes offering a list of sizes safe. */
    if (vbe_read(VBE_XRES) != (u16)w || vbe_read(VBE_YRES) != (u16)h) {
        vbe_write(VBE_ENABLE, VBE_DISABLED);
        return false;
    }

    /* Map the aperture. It sits far above the identity mapped region, so it
       needs page table entries of its own. */
    u64 bytes = (u64)w * 4 * h;
    for (u64 off = 0; off < bytes; off += PAGE_SIZE) {
        if (!map_page(phys + off, phys + off, PTE_PRESENT | PTE_RW)) {
            vbe_write(VBE_ENABLE, VBE_DISABLED);
            return false;
        }
    }

    width = w;
    height = h;
    pitch = w * 4;
    lfb = (u8 *)phys;
    return true;
}

bool fb_init(u32 w, u32 h) {
    active = false;
    via_svga = false;
    adopted = false;

    pci_dev_t vga;
    if (!bga_present(&vga)) return init_svga(w, h);
    if (!bga_mode(w, h)) return false;

    if (!take_back_buffer((u64)pitch * height)) {
        vbe_write(VBE_ENABLE, VBE_DISABLED);
        return false;
    }
    take_sent((u64)pitch * height);

    active = true;
    fb_clear(0);
    fb_flush();
    return true;
}

/* A screen the firmware set up is the size the firmware chose and stays
   that size: the mode was set before ExitBootServices and there is nothing
   left to ask afterwards. Saying so is better than offering a list of
   sizes that all silently do nothing. */
bool fb_mode_settable(void) { return active && !adopted && !via_svga; }

bool fb_set_mode(u32 w, u32 h) {
    if (!fb_mode_settable()) return false;
    if (w < 640 || h < 480 || w > 4096 || h > 4096) return false;
    if (w == width && h == height) return true;

    u32 ow = width, oh = height, opitch = pitch;
    u8 *oback = back, *olfb = lfb;

    if (!bga_mode(w, h)) {
        width = ow; height = oh; pitch = opitch; lfb = olfb;
        bga_mode(ow, oh);                    /* back to one that worked */
        return false;
    }

    /* The new buffer is taken before the old one is let go, so a size the
       heap cannot hold leaves the screen exactly as it was rather than with
       nothing to draw into. */
    u8 *fresh = (u8 *)kmalloc((u64)pitch * height);
    if (!fresh) {
        width = ow; height = oh; pitch = opitch; lfb = olfb;
        bga_mode(ow, oh);
        return false;
    }

    back = fresh;
    if (oback != olfb) kfree(oback);
    take_sent((u64)pitch * height);

    fb_clear(0);
    fb_flush();
    return true;
}

void fb_put(u32 x, u32 y, u32 rgb) {
    if (!active || x >= width || y >= height) return;
    *(u32 *)(back + y * pitch + x * 4) = rgb;
}

u32 fb_get(u32 x, u32 y) {
    if (!active || x >= width || y >= height) return 0;
    return *(u32 *)(back + y * pitch + x * 4);
}

void fb_clear(u32 rgb) {
    if (!active) return;
    u32 *p = (u32 *)back;
    u32 n = width * height;
    for (u32 i = 0; i < n; i++) p[i] = rgb;
}

void fb_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    if (!active) return;
    if (x >= width || y >= height) return;
    if (x + w > width)  w = width - x;
    if (y + h > height) h = height - y;
    for (u32 j = 0; j < h; j++) {
        u32 *row = (u32 *)(back + (y + j) * pitch) + x;
        for (u32 i = 0; i < w; i++) row[i] = rgb;
    }
}

void fb_frame(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    if (!active || w == 0 || h == 0) return;
    fb_rect(x, y, w, 1, rgb);
    fb_rect(x, y + h - 1, w, 1, rgb);
    fb_rect(x, y, 1, h, rgb);
    fb_rect(x + w - 1, y, 1, h, rgb);
}

/* --- getting a frame onto the card -------------------------------------- */
/*
 * The back buffer is three megabytes at 1024x768 and eight at 1080p, and it
 * used to go to the card whole every time anything on the screen changed.
 * Moving the pointer changes something on the screen, so moving the pointer
 * cost a three megabyte write across the bus and, on VMware, an instruction
 * to the host to look at all of it again.
 *
 * That is the whole of "a bit laggy". It is also why giving the machine more
 * of anything did not help: the cost was fixed by the size of the screen
 * rather than by how much of it was different, and it was all on one
 * processor.
 *
 * So a copy of what the card was last given is kept, and each frame is
 * compared against it a band of rows at a time. Only bands that differ are
 * sent. A pointer that moved sends two bands of forty eight; a terminal
 * printing a line sends one; a window being dragged sends the bands it
 * crosses. Nothing above here changed: every drawing routine still draws the
 * whole screen into the back buffer and none of them has to know or say
 * which parts it touched, which is the reason this is done here and not
 * there.
 *
 * The comparison is not free -- it reads both buffers in full -- but it is
 * memory against memory, which is an order of magnitude cheaper than the
 * write across the bus it replaces, and it is the one part of the job that
 * splits cleanly. The bands are independent, so half of them go to another
 * processor when the machine has one to spare. That is the first thing in
 * this kernel a second processor actually does.
 */

/* Sixteen rows: big enough that the per band bookkeeping disappears against
   the copy, small enough that a pointer costs two of them. */
#define BAND_ROWS 16
#define MAX_BANDS 512

static u8  band_dirty[MAX_BANDS];
static u32 band_count;

/* The half given away, and the flag the other processor sets when it has
   finished with it. smp_wait comes back when the work was picked up rather
   than when it was done, and the difference matters when what is being
   waited for is a buffer rather than a count. */
static volatile u32 helper_first, helper_last, helper_done;

/* Guards the band machinery against being entered twice.
 *
 * Only one task composites, but anything can print, and a print draws into
 * the back buffer and flushes. A second caller does what this used to do --
 * sends the whole screen -- which is always correct: it writes the same
 * bytes to the same place and only ever sends more than it had to. */
static volatile bool flushing;

/* What the last frame cost, and how many of them another processor helped
   with. Kept because the claim being made here -- that a frame sends the
   part of the screen that changed rather than all of it -- is exactly the
   kind of claim that is easy to make and easy to be wrong about. cat
   /sys/screen says what actually happened. */
static u64 frames, shared_frames, last_sent, total_sent;

u64 fb_frames(void)        { return frames; }
u64 fb_shared_frames(void) { return shared_frames; }
u64 fb_last_sent(void)     { return last_sent; }
u64 fb_total_sent(void)    { return total_sent; }
u64 fb_screen_bytes(void)  { return (u64)pitch * height; }

static void bands_copy(u32 first, u32 last) {
    for (u32 i = first; i <= last && i < band_count; i++) {
        u32 y = i * BAND_ROWS;
        u32 rows = (height - y) < BAND_ROWS ? (height - y) : BAND_ROWS;
        u64 off = (u64)y * pitch;
        u64 n   = (u64)rows * pitch;

        if (sent_valid && memcmp(sent + off, back + off, n) == 0) {
            band_dirty[i] = 0;
            continue;
        }
        memcpy(lfb + off, back + off, n);
        memcpy(sent + off, back + off, n);
        band_dirty[i] = 1;
    }
}

static void bands_helper(void *arg) {
    (void)arg;
    bands_copy(helper_first, helper_last);
    /* Write combining is allowed to hold the last partial buffer back. The
       processor that waits for this one cannot flush its stores for it. */
    __asm__ volatile ("sfence" ::: "memory");
    __sync_synchronize();
    helper_done = 1;
}

static void whole_screen(void) {
    memcpy(lfb, back, (u64)pitch * height);
    __asm__ volatile ("sfence" ::: "memory");
    last_sent = (u64)pitch * height;
    total_sent += last_sent;
    if (via_svga) svga_update(0, 0, width, height);
}

void fb_flush(void) {
    if (!active) return;

    if (back == lfb) {                       /* drawing straight at the card */
        if (via_svga) svga_update(0, 0, width, height);
        return;
    }

    u64 t0 = rdtsc();
    frames++;

    if (!sent || flushing) { whole_screen(); goto done; }

    band_count = (height + BAND_ROWS - 1) / BAND_ROWS;
    /* Eight thousand rows. No screen is that tall and none is likely to be,
       but a screen this cannot describe has to be sent whole rather than
       have its lower two thirds quietly never arrive. */
    if (band_count > MAX_BANDS) { whole_screen(); goto done; }

    flushing = true;

    {
        /* Half of it on another processor, when there is one free and there
           is enough of it to be worth the interrupt that wakes one. */
        u32 helper = band_count >= 8 ? smp_helper() : 0;
        u32 mine = 0;

        if (helper) {
            helper_first = 0;
            helper_last  = band_count / 2 - 1;
            helper_done  = 0;
            mine = band_count / 2;
            if (!smp_run(helper, bands_helper, 0)) { helper = 0; mine = 0; }
        }

        bands_copy(mine, band_count - 1);
        __asm__ volatile ("sfence" ::: "memory");

        if (helper) {
            /* Bounded, because a processor that does not answer has to cost
               a frame rather than the machine. Its half is then done here,
               which writes the same bytes to the same addresses and is
               therefore safe even if it does wake up later. */
            u32 spin = 0;
            while (!helper_done && spin++ < 20000000u) __asm__ volatile ("pause");
            if (!helper_done) {
                bands_copy(helper_first, helper_last);
                __asm__ volatile ("sfence" ::: "memory");
            } else {
                shared_frames++;
            }
        }
    }

    sent_valid = true;
    flushing = false;

    last_sent = 0;
    for (u32 i = 0; i < band_count; i++) {
        if (!band_dirty[i]) continue;
        u32 y = i * BAND_ROWS;
        u32 rows = (height - y) < BAND_ROWS ? (height - y) : BAND_ROWS;
        last_sent += (u64)rows * pitch;
    }
    total_sent += last_sent;

    if (via_svga) {
        /* Adjacent bands go as one rectangle. The host is being told to look
           again at what it has been given, and telling it twice about two
           halves of one thing costs more than telling it once. */
        u32 i = 0;
        while (i < band_count) {
            if (!band_dirty[i]) { i++; continue; }
            u32 j = i;
            while (j + 1 < band_count && band_dirty[j + 1]) j++;
            u32 y  = i * BAND_ROWS;
            u32 y2 = (j + 1) * BAND_ROWS;
            if (y2 > height) y2 = height;
            svga_update(0, y, width, y2 - y);
            i = j + 1;
        }
    }

done:
    /* The first one is timed, whichever path set the mode up, and the first
       one has nothing to compare against so it is the whole screen. One
       rdtsc against a copy that size is not worth measuring. */
    if (!flush_cycles) flush_cycles = rdtsc() - t0;
}

void fb_flush_rect(u32 x, u32 y, u32 w, u32 h) {
    if (!active) return;
    if (x >= width || y >= height) return;
    if (x + w > width)  w = width - x;
    if (y + h > height) h = height - y;
    if (back != lfb) {
        for (u32 j = 0; j < h; j++) {
            u32 off = (y + j) * pitch + x * 4;
            memcpy(lfb + off, back + off, w * 4);
            /* The mirror has to learn about this too, or the next whole
               frame will decide these rows are already on the screen and
               they will be, and the two will agree about the wrong thing
               the moment anything under them changes. */
            if (sent) memcpy(sent + off, back + off, w * 4);
        }
        __asm__ volatile ("sfence" ::: "memory");
    }
    if (via_svga) svga_update(x, y, w, h);
}

u64 fb_flush_cycles(void) { return flush_cycles; }

bool fb_double_buffered(void) { return active && back != lfb; }
