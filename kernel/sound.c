/* What is in the buffer when the hardware reads it.
 *
 * The controller is never stopped. It reads one buffer round and round for as
 * long as the machine is up, so there is no starting a sound and no stopping
 * one: there is only what happens to be in the buffer at the moment the
 * hardware gets to it. Silence is zeroes. A note is a sine written in ahead of
 * where it has got to.
 *
 * That arrangement has one hazard, which is the whole of the bookkeeping
 * below. A sound written once and left there plays again every time the loop
 * comes round, forever. So everything that has just been played is zeroed
 * behind the playing position, which happens on the timer tick, and a buffer
 * nobody is writing to goes quiet on its own within one pass.
 */
#include "sound.h"
#include "hda.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "sched.h"

/* A third of a second at 48 kHz in stereo. Long enough that a tick's worth of
   jitter cannot empty it, short enough that silencing is not audibly late. */
#define RING_FRAMES  16384

/* Never fill it completely. A writer allowed to catch its own tail would
   overwrite the sample the hardware is reading at that instant. */
#define GUARD_FRAMES 256

static bool  ready;
static i16  *ring;
static u32   ring_bytes;
static u32   frame_bytes;

/* Both counted in bytes and never wrapped, so the distance between them is
   just a subtraction. The hardware's own pointer does wrap, and turning that
   into this is what advance does. */
static u64 played;
static u64 written;
static u64 zeroed;
static u32 last_pos;

/* Everything written goes through this. Seventy rather than a hundred
   because a machine that comes up at full volume is a machine somebody
   turns down once and resents twice. */
static u32 volume = 70;

u32 sound_volume(void) { return volume; }

void sound_set_volume(u32 percent) {
    if (percent > 100) percent = 100;
    volume = percent;
}

/* The copy into the ring, quieter. At full volume it is the memcpy it was. */
static void copy_at(i16 *dst, const i16 *src, u32 samples) {
    if (volume >= 100) { memcpy(dst, src, (u64)samples * 2); return; }
    if (!volume)       { memset(dst, 0, (u64)samples * 2); return; }
    for (u32 i = 0; i < samples; i++)
        dst[i] = (i16)(((i32)src[i] * (i32)volume) / 100);
}

bool sound_present(void)  { return ready; }
u32  sound_rate(void)     { return hda_rate(); }
u32  sound_channels(void) { return hda_channels(); }

const char *sound_describe(void) {
    return ready ? hda_describe() : "none";
}

/* A sine, without a floating point unit or a table.
 *
 * A parabola through zero, a peak and zero again is within a few percent of a
 * sine over half a period, and the ear cannot tell the difference at a single
 * frequency. The phase is sixteen bits of a full turn, so the top bit is
 * which half of the wave this is and the rest is how far through it. */
static i16 isin(u16 phase) {
    u32 half = phase & 0x7FFF;
    i32 y = (i32)((half * (32768u - half)) >> 13);
    if (y > 32767) y = 32767;
    return (i16)((phase & 0x8000) ? -y : y);
}

/* Where the hardware has got to, as a number that only goes up. */
static void advance(void) {
    if (!ready) return;
    u32 pos = hda_position();
    if (pos >= ring_bytes) pos %= ring_bytes;

    played += (pos >= last_pos) ? (pos - last_pos)
                                : (ring_bytes - last_pos + pos);
    last_pos = pos;

    /* Nothing was queued, so what is in front of the hardware is whatever was
       played last time round. Keep the written mark up with the playing one
       and the zeroing below turns it into silence. */
    if (written < played) written = played;
}

void sound_poll(void) {
    if (!ready) return;
    advance();

    /* Everything that is not queued audio has to be silence, all the way
       round the buffer, and not merely the part just played.
     *
       Zeroing only behind the playing position looks right and is not: when
       a sound ends, the rest of the buffer in front of it still holds the
       sound, and the hardware walks straight into it and plays it again
       before anything gets round to clearing it. What that sounds like is
       every noise ending with a stutter of itself, and what it measured as
       was two notes running into one.
     *
       So the region cleared is from the end of what is queued, forward, all
       the way to where the hardware will be in one pass. Almost every tick
       that is the couple of kilobytes played since the last one. It is the
       whole buffer exactly once, just after a sound ends. */
    u64 front = written > played ? written : played;
    u64 limit = played + ring_bytes;
    if (zeroed < front) zeroed = front;

    u32 budget = ring_bytes;
    while (zeroed < limit && budget) {
        u32 at = (u32)(zeroed % ring_bytes);
        u32 run = ring_bytes - at;
        u64 left = limit - zeroed;
        if (run > left) run = (u32)left;
        if (run > budget) run = budget;

        memset((u8 *)ring + at, 0, run);
        zeroed += run;
        budget -= run;
    }
}

u32 sound_write(const i16 *frames, u32 count) {
    if (!ready || !frames || !count) return 0;

    const u8 *src = (const u8 *)frames;
    u32 want = count * frame_bytes;
    u32 done = 0;

    /* Room only appears when the hardware consumes what is already there, so
       a stream that is not running never makes any and a caller waiting for
       it waits for good. That is not a theoretical worry: it is what a stream
       descriptor set up but never started looks like, and it hung the shell.
       Half a second without the play position moving is taken as stopped. */
    u64 seen = played;
    u32 stalled = 0;

    while (done < want) {
        advance();

        /* How much of the buffer is not either queued already or too close to
           what is being read right now. */
        u64 queued = written - played;
        u64 room = (u64)ring_bytes - queued - (u64)GUARD_FRAMES * frame_bytes;
        if ((i64)room <= 0) {
            if (played != seen) {
                seen = played;
                stalled = 0;
            } else if (++stalled > 250) {
                break;
            }
            /* The only way to make room is to wait for it to be played. */
            sleep_ms(2);
            continue;
        }

        u32 chunk = (u32)room;
        if (chunk > want - done) chunk = want - done;

        u32 at = (u32)(written % ring_bytes);
        u32 run = ring_bytes - at;
        if (run > chunk) run = chunk;

        /* Claimed before it is filled. The silencing above runs from the
           timer, so it can land in the middle of this copy, and if it were
           to see the old mark it would zero the bytes being written. */
        written += run;
        copy_at((i16 *)((u8 *)ring + at), (const i16 *)(src + done), run / 2);
        done += run;

        if (zeroed < written) zeroed = written;
    }
    return done / frame_bytes;
}

void sound_tone(u32 hz, u32 ms) {
    if (!ready || !hz || !ms) return;

    u32 chans = hda_channels();
    u32 rate = hda_rate();
    u32 total = (rate * ms) / 1000;

    /* A block at a time, because the buffer is smaller than most notes and
       sound_write would otherwise be waiting inside a single call for the
       whole length of it. */
    i16 block[512 * 2];
    u32 per_block = (sizeof block / sizeof block[0]) / chans;

    u32 phase = 0;
    u32 step = (u32)(((u64)hz << 16) / rate);

    while (total) {
        u32 n = total < per_block ? total : per_block;
        for (u32 i = 0; i < n; i++) {
            i16 s = isin((u16)(phase >> 0));
            phase = (phase + step) & 0xFFFF;
            for (u32 c = 0; c < chans; c++) block[i * chans + c] = s;
        }
        sound_write(block, n);
        total -= n;
    }
}

void sound_silence(void) {
    if (!ready) return;
    memset(ring, 0, ring_bytes);
    advance();
    written = played;
    zeroed = played;
}

bool sound_init(void) {
    ready = false;
    if (!hda_init()) return false;

    frame_bytes = hda_frame_bytes();
    ring_bytes = RING_FRAMES * frame_bytes;

    ring = (i16 *)kmalloc(ring_bytes);
    if (!ring) return false;
    memset(ring, 0, ring_bytes);

    if (!hda_start(ring, ring_bytes, 48000)) return false;

    played = written = zeroed = 0;
    last_pos = hda_position() % ring_bytes;
    ready = true;
    return true;
}
