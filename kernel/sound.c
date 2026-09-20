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
#include "ens.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "sched.h"
#include "io.h"

/* A third of a second at 48 kHz in stereo. Long enough that a tick's worth of
   jitter cannot empty it, short enough that silencing is not audibly late. */
#define RING_FRAMES  16384

/* Never fill it completely. A writer allowed to catch its own tail would
   overwrite the sample the hardware is reading at that instant. */
#define GUARD_FRAMES 256

/* Which controller answered.
 *
 * Two, and they have nothing in common but the shape of the problem: a
 * buffer that plays forever and a position to stay ahead of. HD Audio is
 * what every machine made this century has and what QEMU offers; the
 * Ensoniq is what VMware gives a guest whose kind it does not recognise,
 * which is this one. Picking between them here rather than inside each
 * driver means everything below is written once. */
typedef enum { SND_NONE, SND_HDA, SND_ENS } snd_dev_t;
static snd_dev_t device;

static u32 snd_rate(void) {
    return device == SND_HDA ? hda_rate() : device == SND_ENS ? ens_rate() : 0;
}
static u32 snd_channels(void) {
    return device == SND_HDA ? hda_channels()
         : device == SND_ENS ? ens_channels() : 0;
}
static u32 snd_frame_bytes(void) {
    return device == SND_HDA ? hda_frame_bytes()
         : device == SND_ENS ? ens_frame_bytes() : 0;
}
static u32 snd_position(void) {
    return device == SND_HDA ? hda_position()
         : device == SND_ENS ? ens_position() : 0;
}
static const char *snd_name(void) {
    return device == SND_HDA ? hda_describe()
         : device == SND_ENS ? ens_describe() : "none";
}
static bool snd_start(void *buf, u32 bytes, u32 rate) {
    return device == SND_HDA ? hda_start(buf, bytes, rate)
         : device == SND_ENS ? ens_start(buf, bytes, rate) : false;
}

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
u64 sound_played(void) { return played; }


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
u32  sound_rate(void)     { return snd_rate(); }
u32  sound_channels(void) { return snd_channels(); }

const char *sound_describe(void) {
    return ready ? snd_name() : "none";
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

/* Whether the hardware will say where it has got to at all.
 *
 * VMware's Ensoniq plays the buffer perfectly well and never updates the
 * field that says how far through it is. It raises the interrupt that means
 * a buffer went out and leaves the current count at zero for as long as the
 * machine is on, which from here is indistinguishable from a controller that
 * was set up and never started.
 *
 * It is not indistinguishable in its consequences, and they are bad in both
 * directions. Nothing is ever counted as played, so the buffer is never
 * silenced behind the hardware and whatever was last written to it repeats
 * forever; and nothing ever frees room, so a writer waiting for some waits
 * for good and every note longer than the buffer is cut off.
 *
 * So when the register has not moved after a quarter of a second of the
 * machine being up, the clock is used instead: the rate is known, the tick
 * is known, and the product is where the hardware must be. It drifts, it
 * cannot be resynchronised against anything, and it is a great deal better
 * than a buffer that never empties. sound_clocked says which it is, and the
 * boot log prints it, because a machine keeping time for its own sound card
 * is worth knowing about. */
static bool pos_moves;        /* the register has been seen to change */
static bool clocked;          /* and when it never does, this takes over */
static u64  clock_from_tick;
static u64  clock_from_played;
static u64  started_at;

bool sound_clocked(void) { return clocked; }

/* Where the hardware has got to, as a number that only goes up.
 *
 * Read and updated with nothing else running, which is the whole of it.
 *
 * This is called from two places -- the timer, which silences what has been
 * played, and whoever is writing audio -- and it reads the hardware's
 * position, compares it against the last one, and stores the new one. Let the
 * timer land between the read and the store and the writer comes back with a
 * position from before the interrupt and a mark from after it, which is a
 * position that has gone backwards, which this reads as the buffer having
 * wrapped all the way round: the play mark jumps forward by a whole ring.
 *
 * What that does is free the entire buffer, so a writer that should have
 * waited for room does not. A note longer than the third of a second the ring
 * holds was written into it three or four times over at memory speed, each
 * pass landing on top of the one before, and what came out was the last third
 * of the note and nothing else. sound_tone said it blocks for as long as the
 * note lasts; it was returning in three percent of that. */
static void advance(void) {
    if (!ready) return;

    bool were_on = interrupts_enabled();
    if (were_on) cli();

    u32 pos = snd_position();
    if (pos >= ring_bytes) pos %= ring_bytes;
    /* Only worth asking while the answer can still change anything: once the
       clock has taken over, last_pos is derived from it and no longer has
       anything to say about the register. */
    if (!clocked && pos != last_pos) pos_moves = true;

    if (!pos_moves && !clocked &&
        timer_ticks() - started_at > (u64)timer_hz() / 4) {
        clocked = true;
        clock_from_tick = timer_ticks();
        clock_from_played = played;
    }

    if (clocked) {
        /* Ticks since the fallback took over, turned into bytes at the rate
           the stream is running. Never allowed to go backwards. */
        u64 since = timer_ticks() - clock_from_tick;
        u64 want = clock_from_played +
                   since * (u64)snd_rate() * (u64)frame_bytes / (u64)timer_hz();
        if (want > played) played = want;
        last_pos = (u32)(played % ring_bytes);
    } else {
        played += (pos >= last_pos) ? (pos - last_pos)
                                    : (ring_bytes - last_pos + pos);
        last_pos = pos;
    }

    /* Nothing was queued, so what is in front of the hardware is whatever was
       played last time round. Keep the written mark up with the playing one
       and the zeroing below turns it into silence. */
    if (written < played) written = played;

    if (were_on) sti();
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
       Half a second without the play position moving is taken as stopped,
       and half a second is fifty of these now that the sleep below sleeps. */
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
            } else if (++stalled > 50) {
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

    u32 chans = snd_channels();
    u32 rate = snd_rate();
    u32 total = (rate * ms) / 1000;
    if (!total) return;

    /* A block at a time, because the buffer is smaller than most notes and
       sound_write would otherwise be waiting inside a single call for the
       whole length of it. */
    i16 block[512 * 2];
    u32 per_block = (sizeof block / sizeof block[0]) / chans;

    u32 phase = 0;
    u32 step = (u32)(((u64)hz << 16) / rate);

    /* A note that begins and ends at full height steps the cone, and a step
       is a click. Five milliseconds at each end takes it out. That is far too
       short to hear as a fade and it is the difference between a note and a
       note with a tick on both sides of it, which is very audible on short
       ones -- and the shortest note here is the one the volume slider plays,
       where the tick would be most of what there was. */
    u32 ramp = rate / 200;
    if (ramp > total / 2) ramp = total / 2;

    u32 done = 0;
    u32 left = total;

    while (left) {
        u32 n = left < per_block ? left : per_block;
        for (u32 i = 0; i < n; i++) {
            i32 s = isin((u16)(phase >> 0));
            phase = (phase + step) & 0xFFFF;

            u32 at = done + i;
            if (ramp) {
                if (at < ramp)                 s = s * (i32)at / (i32)ramp;
                else if (at >= total - ramp)   s = s * (i32)(total - at) / (i32)ramp;
            }
            for (u32 c = 0; c < chans; c++) block[i * chans + c] = (i16)s;
        }
        sound_write(block, n);
        done += n;
        left -= n;
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
    device = SND_NONE;

    /* HD Audio first, because a machine that has both has the better one.
       Nothing here depends on the order beyond that. */
    if (hda_init())      device = SND_HDA;
    else if (ens_init()) device = SND_ENS;
    else return false;

    frame_bytes = snd_frame_bytes();
    if (!frame_bytes) { device = SND_NONE; return false; }
    ring_bytes = RING_FRAMES * frame_bytes;

    ring = (i16 *)kmalloc(ring_bytes);
    if (!ring) { device = SND_NONE; return false; }
    memset(ring, 0, ring_bytes);

    /* Forty eight thousand is asked for and whatever is agreed to is what is
       used: the Ensoniq's older half makes its rate by dividing a crystal
       and cannot produce this one at all. sound_rate reports what happened
       rather than what was wanted, and everything that makes a sound reads
       it. */
    if (!snd_start(ring, ring_bytes, 48000)) { device = SND_NONE; return false; }

    played = written = zeroed = 0;
    last_pos = snd_position() % ring_bytes;
    pos_moves = false;
    clocked = false;
    started_at = timer_ticks();
    ready = true;
    return true;
}
