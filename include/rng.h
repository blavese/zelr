#pragma once
#include "types.h"

/* Unpredictable bytes.
 *
 * This is the quietest way for a cryptographic system to be worthless. Every
 * other mistake here shows up as a failure to connect; this one shows up as
 * a connection that works perfectly and that somebody else can read, because
 * the private key was guessable. There is nothing to notice.
 *
 * So the seed is taken from as many independent things as the machine will
 * give: the processor's own generator where it exists, the cycle counter
 * sampled against the interrupt timer, and the clock. They are mixed rather
 * than chosen between, so that a source being useless weakens the result and
 * does not decide it.
 *
 * rng_ready() says whether any of it was actually unpredictable. A caller
 * that needs a key must refuse to make one when it is false rather than
 * proceed with something that looks like a key. */
void rng_init(void);
bool rng_ready(void);

/* Where the entropy came from, for the boot log: "rdseed", "rdrand",
   "timing", or "none", with the sources joined. */
const char *rng_sources(void);

void rng_bytes(u8 *out, u32 len);

/* Mixes something in. Anything may be added at any time; it can only help. */
void rng_stir(const void *data, u32 len);

/* One cycle counter sample, from the timer interrupt.
 *
 * The jitter source cannot be collected at rng_init: it is the variation in
 * how many cycles pass between one tick and the next, and at the point the
 * machine is still setting itself up there are no ticks, because interrupts
 * are not enabled until the scheduler starts. Sampling in the interrupt
 * itself is also the honest place to do it, since that is where the variation
 * being measured actually happens. Cheap enough to do every tick, which at
 * 100 Hz means the pool is unpredictable long before anything asks. */
void rng_tick(void);
