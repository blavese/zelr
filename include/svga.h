#pragma once
#include "types.h"

/* VMware's SVGA II adapter, which is the only way to get a framebuffer on
   VMware Workstation and Fusion.
 *
 * Everything else this kernel has met puts a mode on the screen through the
 * Bochs VBE dispatch ports: QEMU's std VGA implements them, and VirtualBox
 * implements them too under its own device id. VMware does not implement
 * them at all, so fb_init found nothing to ask and the machine fell back to
 * VGA text, which is a working shell and no desktop.
 *
 * The device is told about a mode through a pair of I/O ports, and the
 * pixels live in an aperture it names itself. Unlike a plain linear
 * framebuffer, writing to that aperture is not enough: the adapter has to be
 * told which part of it changed, through a command queue in memory. */

typedef struct {
    u64 fb_phys;        /* where the pixels are */
    u32 fb_bytes;       /* how much of the aperture is the visible frame */
    u32 pitch;          /* bytes per scanline, as the device reports it */
    u32 width, height;
} svga_mode_t;

/* Finds the adapter and puts it in this mode. False when it is not there,
   which is every machine that is not VMware. */
bool svga_init(u32 width, u32 height, svga_mode_t *out);

bool svga_present(void);

/* Says which pixels changed. Without this the aperture holds the new frame
   and the screen goes on showing the old one. */
void svga_update(u32 x, u32 y, u32 w, u32 h);
