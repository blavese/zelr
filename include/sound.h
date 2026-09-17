#pragma once
#include "types.h"

/* Sound, above the controller.
 *
 * kernel/hda.c gets a buffer playing on a loop and never stops it. Everything
 * here is about what is in that buffer at the moment the hardware reads it:
 * writing samples in ahead of where it has got to, and putting silence back
 * behind it so that nothing plays twice.
 *
 * Samples are signed sixteen bit, interleaved, at whatever rate the hardware
 * agreed to, which sound_rate reports rather than anybody assuming. */

bool sound_init(void);
bool sound_present(void);
const char *sound_describe(void);

/* How loud, as a percentage, applied to everything on its way into the
   buffer. Zero is silence and is how mute is spelt. */
u32  sound_volume(void);
void sound_set_volume(u32 percent);

u32 sound_rate(void);
u32 sound_channels(void);

/* Writes frames, waiting when the buffer is full, and returns how many went
   in. A frame is one sample for every channel. */
u32 sound_write(const i16 *frames, u32 count);

/* A note. Blocks for about as long as the note lasts, because it is writing
   into a buffer that only drains in real time. */
void sound_tone(u32 hz, u32 ms);

/* Stops whatever is queued, now, rather than letting it finish. */
void sound_silence(void);

/* Puts silence behind the playing position, so a sound that has finished is
   not still in the buffer when the loop comes round again. Called from the
   timer, like the other things that have to keep happening. */
void sound_poll(void);
