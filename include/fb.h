#pragma once
#include "types.h"

/* Linear framebuffer, set up through the Bochs VBE dispatch interface that
   QEMU's std VGA implements. */

bool fb_init(u32 width, u32 height);

/* Takes over a screen a bootloader already set up, which is the only option
   on a machine with no VBE to ask. */
bool fb_adopt(u64 base, u32 width, u32 height, u32 pitch_pixels);
bool fb_active(void);

/* Changing the size while the machine is running.
 *
 * Only where the mode is ours to set. A framebuffer the firmware handed
 * over cannot be changed at all once its boot services are gone, which is
 * every UEFI machine, so this says so rather than pretending. */
bool fb_mode_settable(void);
bool fb_set_mode(u32 w, u32 h);

/* Which of the three ways the screen was obtained, for the boot log. The
   log used to say "vbe" whichever it had been, which is wrong on VMware
   and is exactly the sort of reporting that sends someone looking in the
   wrong place. */
const char *fb_backend(void);

u32  fb_width(void);
u32  fb_height(void);
u32  fb_pitch(void);       /* bytes per scanline */
u8  *fb_pixels(void);

void fb_clear(u32 rgb);
void fb_put(u32 x, u32 y, u32 rgb);
u32  fb_get(u32 x, u32 y);
void fb_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb);
void fb_frame(u32 x, u32 y, u32 w, u32 h, u32 rgb);

/* Everything is drawn into a back buffer; this pushes it to the card. */
void fb_flush(void);

/* What the full screen copy at startup cost, in cycles. */
u64 fb_flush_cycles(void);

/* False when the back buffer would not fit and drawing goes straight at the
   screen, which is visible as tearing and as every draw costing bus time. */
bool fb_double_buffered(void);
void fb_flush_rect(u32 x, u32 y, u32 w, u32 h);

#define RGB(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))
