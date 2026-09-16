#pragma once
#include "types.h"

/* Intel High Definition Audio.
 *
 * The sound controller every machine made this century has. AC'97 and the
 * Sound Blasters are easier to drive and neither exists on a laptop, so this
 * is the one worth writing.
 *
 * It is two separate things wearing one name. The controller is a PCI device
 * that does nothing but move bytes out of memory on a schedule. The codec is
 * a small graph of widgets on a serial link behind it, and what it does with
 * those bytes depends on how the graph is wired, which differs on every
 * machine. So the driver below is half DMA and half walking a graph looking
 * for a route from a converter to a socket somebody might have plugged a
 * speaker into. */

bool hda_init(void);
bool hda_present(void);

/* What was found, for the boot log: the codec's vendor, and which widgets the
   route ended up using. */
const char *hda_describe(void);

/* Starts the controller reading a buffer over and over, forever. Sound is
   made by writing into that buffer ahead of where it has got to, and silence
   by leaving zeroes there, which is why there is no stop. */
bool hda_start(void *buffer, u32 bytes, u32 rate_hz);

/* How far through the buffer it has got, in bytes. Wraps with the buffer. */
u32 hda_position(void);

/* Samples per second, channels, and bytes in one frame of both. Fixed at
   setup and reported rather than assumed, because a codec is allowed to
   refuse a rate. */
u32 hda_rate(void);
u32 hda_channels(void);
u32 hda_frame_bytes(void);
