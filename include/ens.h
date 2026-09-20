#pragma once
#include "types.h"

/* Ensoniq AudioPCI: the ES1370 and the ES1371.
 *
 * This is the sound card VMware gives a machine whose guest type it does not
 * recognise, which is every machine running something written from scratch.
 * It offers HD Audio only to a guest it knows, so without this the most
 * likely way anybody runs zelr has a sound controller and no sound.
 *
 * The interface is the same shape as hda.h, because what is above it does
 * not care which of them is there: a buffer that plays on a loop forever,
 * and a position to write ahead of.
 */

bool ens_init(void);
bool ens_present(void);

/* Which of the two it turned out to be, and at what rate, for the boot log. */
const char *ens_describe(void);

bool ens_start(void *buffer, u32 bytes, u32 rate_hz);
u32  ens_position(void);

u32  ens_rate(void);
u32  ens_channels(void);
u32  ens_frame_bytes(void);
