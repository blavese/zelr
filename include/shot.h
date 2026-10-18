#pragma once
#include "types.h"

/* A picture of the screen, kept as a PNG.
 *
 * Print Screen on the desktop (wm.c) saves what the screen shows into
 * /home/pictures, named by the local time ("screen 2026-10-18 19.04.05.png").
 * PNG because every browser reads it, this one included; the deflate inside is
 * this file's own, since the kernel had only ever inflated. */

/* Saves the screen as it is now. The path written, or 0 when it could not be
   (no memory, no disk); `why` says which. */
const char *shot_save(const char **why);

/* Deflate (RFC 1951) of n bytes into out, a single block with the fixed
   codes: the size written, or 0 when cap is too small. For the self test,
   which inflates it back. */
u32 shot_deflate(const u8 *in, u32 n, u8 *out, u32 cap);

/* A whole PNG of a w by h picture whose pixels are 0x00RRGGBB, a row at a
   time from row(y, arg); into a buffer it allocates, its size in *len. 0
   without the memory. The caller frees it. */
u8 *shot_png(u32 w, u32 h, const u32 *(*row)(u32 y, void *arg), void *arg, u32 *len);
