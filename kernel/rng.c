/* The random number generator.
 *
 * A hash based construction: a pool of state, stirred with whatever comes
 * in, and output taken as the hash of the pool and a counter. Output never
 * reveals the pool, and the pool moves on after every request, so bytes
 * handed out once cannot be produced again by asking again.
 *
 * The seeding is the part that matters. RDSEED is the processor's own
 * entropy source and RDRAND the generator built on it; where they exist
 * they are used, and where they do not the fallback is timing: the cycle
 * counter sampled across the interrupt timer's ticks, whose low bits vary
 * by an amount nothing in the machine controls. That is a weaker source and
 * it is treated as one. It is also the only one an emulated machine usually
 * has, because the default processor QEMU presents has neither instruction,
 * so it is not a corner worth leaving half done.
 *
 * Everything is mixed. A source that turns out to be constant then weakens
 * the pool rather than fixing it, which is the difference between a
 * degraded generator and a predictable one. */
#include "rng.h"
#include "sha256.h"
#include "io.h"
#include "rtc.h"
#include "string.h"

/* How many timer ticks have to have been sampled before the jitter they
   carry is counted as a source. Each one contributes the low bits of the
   cycle count between two interrupts, which is a fraction of a bit; this
   many is a second and a bit at 100 Hz, and the machine spends longer than
   that reaching a point where anything asks. */
#define JITTER_ENOUGH 128

static u8  pool[SHA256_SIZE];
static u64 counter;
static bool have_hardware;
static char sources[40];

static u64 jitter_last;
static u64 jitter_spread;
static u32 jitter_samples;

static bool cpu_has(u32 leaf, int reg, u32 bit) {
    u32 a, b, c, d;
    cpuid_read(0, &a, &b, &c, &d);
    if (a < leaf) return false;
    cpuid_read(leaf, &a, &b, &c, &d);
    u32 v = reg == 0 ? a : reg == 1 ? b : reg == 2 ? c : d;
    return (v >> bit) & 1;
}

/* Both of these can fail on a given attempt by design, so they are asked
   more than once before being given up on. */
static bool try_rdseed(u64 *out) {
    u64 v; u8 ok;
    for (int i = 0; i < 32; i++) {
        __asm__ volatile ("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return true; }
    }
    return false;
}

static bool try_rdrand(u64 *out) {
    u64 v; u8 ok;
    for (int i = 0; i < 32; i++) {
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return true; }
    }
    return false;
}

/* The pool is read and rewritten whole, and one of the things that stirs it
   is the timer interrupt, so a task doing the same can be cut in half by it.
   Neither outcome is dangerous — losing one contribution does not make a
   pool guessable — but the two writers would interleave into a pool that is
   half of one hash and half of another, which is a state nothing intends and
   nothing would notice. Interrupts are off for the length of one hash of
   forty bytes.

   Restored rather than simply enabled: this is called during startup with
   interrupts already off, and turning them on there would let the first
   timer tick arrive in the middle of a kernel that is not ready for one. */
static void stir_locked(const void *data, u32 len) {
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, pool, sizeof(pool));
    sha256_update(&s, data, len);
    sha256_final(&s, pool);
}

void rng_stir(const void *data, u32 len) {
    bool on = interrupts_enabled();
    if (on) cli();
    stir_locked(data, len);
    if (on) sti();
}

/* The cycle counter against the interrupt timer. The timer fires on its own
   schedule and the count of cycles between one tick and the next varies in
   its low bits by an amount no part of this machine decides. One sample is
   worth very little; the point of taking many is that the variation
   accumulates.
 *
 * This runs inside the timer interrupt, which is the only place the interval
 * being measured can be seen. Waiting for ticks in a loop at startup, which
 * is the obvious way to write this, cannot work: interrupts are not enabled
 * until the scheduler starts, so the loop would spin until it gave up and
 * report that the machine has no entropy at all. */
void rng_tick(void) {
    u64 now = rdtsc();
    u64 delta = now - jitter_last;
    jitter_last = now;

    /* The first sample is measured from zero and is the whole cycle count
       since reset rather than an interval, so it says nothing about
       variation. Mixed in anyway, and not counted.

       Stirred with the version that does not touch the interrupt flag: this
       is already inside an interrupt, and the other one would turn
       interrupts back on in the middle of one. */
    if (jitter_samples || delta) stir_locked(&delta, sizeof(delta));
    if (jitter_samples) jitter_spread |= delta ^ (delta >> 17);
    if (jitter_samples < JITTER_ENOUGH * 2) jitter_samples++;
}

/* If every interval came out identical the counter is not varying and this
   source gave nothing, whatever it looked like. */
static bool timing_ok(void) {
    return jitter_samples >= JITTER_ENOUGH && (jitter_spread & 0xffff) != 0;
}

static bool have_rdseed, have_rdrand;

void rng_init(void) {
    memset(pool, 0, sizeof(pool));
    counter = 0;
    have_hardware = false;
    have_rdseed = have_rdrand = false;
    jitter_last = jitter_spread = 0;
    jitter_samples = 0;
    sources[0] = 0;

    /* Anything at all to start with, none of it unpredictable, all of it
       harmless to include. */
    u64 t = rdtsc();
    rng_stir(&t, sizeof(t));
    rtc_time_t now;
    if (rtc_read(&now)) rng_stir(&now, sizeof(now));

    if (cpu_has(7, 1, 18)) {                 /* rdseed */
        bool any = false;
        for (int i = 0; i < 16; i++) {
            u64 v;
            if (!try_rdseed(&v)) break;
            rng_stir(&v, sizeof(v));
            any = true;
        }
        if (any) { have_hardware = have_rdseed = true; }
    }

    if (cpu_has(1, 2, 30)) {                 /* rdrand */
        bool any = false;
        for (int i = 0; i < 16; i++) {
            u64 v;
            if (!try_rdrand(&v)) break;
            rng_stir(&v, sizeof(v));
            any = true;
        }
        if (any) { have_hardware = have_rdrand = true; }
    }
}

bool rng_ready(void) { return have_hardware || timing_ok(); }

/* Built when asked rather than at startup, because the timing source is not
   there yet when the rest are counted: it arrives a second or so after the
   scheduler begins, and a string fixed before that would say "none" on every
   machine whose only source is the one it actually has. */
const char *rng_sources(void) {
    const char *names[3];
    u32 nn = 0, w = 0;

    if (have_rdseed) names[nn++] = "rdseed";
    if (have_rdrand) names[nn++] = "rdrand";
    if (timing_ok()) names[nn++] = "timing";
    if (nn == 0) return "none";

    for (u32 i = 0; i < nn; i++) {
        if (i && w < sizeof(sources) - 1) sources[w++] = '+';
        for (const char *p = names[i]; *p && w < sizeof(sources) - 1; p++)
            sources[w++] = *p;
    }
    sources[w] = 0;
    return sources;
}

void rng_bytes(u8 *out, u32 len) {
    while (len) {
        /* Output is the hash of the pool and a counter; the pool is then
           moved on, so the same bytes cannot be asked for twice. */
        u8 block[SHA256_SIZE], snapshot[SHA256_SIZE];
        u64 n;

        /* A copy, for the same reason the stir takes the pool whole: the
           timer interrupt rewrites it, and hashing it byte by byte while
           that happens would hash half of one pool and half of the next. */
        bool on = interrupts_enabled();
        if (on) cli();
        memcpy(snapshot, pool, sizeof(snapshot));
        n = counter++;
        if (on) sti();

        sha256_t s;
        sha256_init(&s);
        sha256_update(&s, snapshot, sizeof(snapshot));
        sha256_update(&s, &n, sizeof(n));
        sha256_final(&s, block);

        u32 take = len < SHA256_SIZE ? len : SHA256_SIZE;
        memcpy(out, block, take);
        out += take;
        len -= take;

        u8 step = 1;
        rng_stir(&step, 1);
        u64 t = rdtsc();
        rng_stir(&t, sizeof(t));
    }
}
