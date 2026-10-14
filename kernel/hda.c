/* Intel HD Audio: the controller, and enough of a codec walk to find a way out.
 *
 * The controller part is small. It is a ring of buffer descriptors, a stream
 * descriptor pointing at them, and a run bit. Once it is running it reads the
 * buffer forever and whatever is in the buffer is what comes out of the
 * speaker, so making a sound is writing into memory at the right moment and
 * making silence is leaving it alone.
 *
 * The codec part is where the work is. Behind the controller is a graph of
 * widgets: converters that turn a stream into an analogue signal, mixers and
 * selectors that route between them, and pin complexes that are the physical
 * sockets and the internal speaker. Nothing says which of them is wired to
 * what you can hear. The firmware fills in a configuration default on each
 * pin describing what the manufacturer soldered it to, and the driver has to
 * read those, pick one, and then walk backwards along the connection lists
 * until it reaches a converter. That walk is why a machine can have working
 * audio hardware and no sound.
 *
 * Commands go to the codec through a pair of rings rather than the immediate
 * command registers, which are simpler and are also optional and missing on a
 * good deal of real hardware.
 */
#include "hda.h"
#include "pci.h"
#include "paging.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "io.h"
#include "blackbox.h"

/* --- controller registers, offsets from the memory window ---------------- */
#define GCAP        0x00      /* how many streams of each kind it has */
#define GCTL        0x08
#define STATESTS    0x0E      /* which codecs answered the reset */
#define INTCTL      0x20
#define CORBLBASE   0x40
#define CORBUBASE   0x44
#define CORBWP      0x48
#define CORBRP      0x4A
#define CORBCTL     0x4C
#define CORBSIZE    0x4E
#define RIRBLBASE   0x50
#define RIRBUBASE   0x54
#define RIRBWP      0x58
#define RINTCNT     0x5A
#define RIRBCTL     0x5C
#define RIRBSTS     0x5D
#define RIRBSIZE    0x5E
#define DPLBASE     0x70
#define DPUBASE     0x74

#define GCTL_RESET  (1u << 0)
#define CORBRP_RST  (1u << 15)
#define CORBCTL_RUN (1u << 1)
#define RIRBCTL_RUN (1u << 1)
/* Response interrupt enable. Nothing here wants an interrupt: the global
   interrupt control is left off, so none is ever delivered. The bit is set
   because the controller only raises the status flag when it is, and only
   counts responses as acknowledged when that flag is cleared. Without it the
   response counter reaches RINTCNT, is never reset, and the controller stops
   fetching commands for good. One command works and every command after it
   times out, which reads exactly like a codec that is not there. */
#define RIRBCTL_IRQ (1u << 0)

/* A stream descriptor is a fixed block, and they sit end to end after the
   controller's own registers. Input streams come first, so the first output
   one is at the number of input streams the controller says it has. */
#define SD_BASE     0x80
#define SD_STRIDE   0x20
#define SD_CTL      0x00
#define SD_STS      0x03
#define SD_LPIB     0x04
#define SD_CBL      0x08
#define SD_LVI      0x0C
#define SD_FMT      0x12
#define SD_BDPL     0x18
#define SD_BDPU     0x1C

#define SD_CTL_SRST (1u << 0)
#define SD_CTL_RUN  (1u << 1)

/* --- codec verbs ---------------------------------------------------------
 *
 * Twenty bits of a command word. The twelve bit ones carry a byte of payload
 * and the four bit ones carry two, which is the only reason they look so
 * different from each other. */
#define GET_PARAM(p)        (0xF0000u | (p))
#define SET_POWER(s)        (0x70500u | (s))
#define GET_CONN(i)         (0xF0200u | (i))
#define SET_CONN_SEL(i)     (0x70100u | (i))
#define SET_PIN_CTL(v)      (0x70700u | (v))
#define SET_EAPD(v)         (0x70C00u | (v))
#define SET_STREAM_CHAN(v)  (0x70600u | (v))
#define GET_CONFIG_DEFAULT  (0xF1C00u)
#define SET_FORMAT(v)       (0x20000u | (v))
#define SET_AMP(v)          (0x30000u | (v))

/* Parameters, read with GET_PARAM. */
#define P_VENDOR        0x00
#define P_NODE_COUNT    0x04
#define P_FUNC_TYPE     0x05
#define P_WIDGET_CAPS   0x09
#define P_PIN_CAPS      0x0C
#define P_IN_AMP_CAP    0x0D
#define P_CONN_LEN      0x0E
#define P_OUT_AMP_CAP   0x12

/* The widget describes its own amplifiers; without it the function group's
   description stands for them. */
#define WCAP_AMP_OVERRIDE (1u << 3)

#define WIDGET_DAC      0
#define WIDGET_MIXER    2
#define WIDGET_SELECTOR 3
#define WIDGET_PIN      4

#define PIN_CAP_OUTPUT  (1u << 4)

/* An amplifier on: the output side or an input by its index, both channels,
   not muted, at gain g. */
#define AMP_OUT(g)      (0xB000u | (g))
#define AMP_IN(i, g)    (0x7000u | ((i) << 8) | (g))

#define PIN_OUT_ENABLE  0xC0      /* output, and headphone drive with it */

#define MAX_WIDGETS     64
#define RING_ENTRIES    256

static bool present;
static volatile u8 *regs;
static u32 out_stream;            /* which stream descriptor is ours */
static u32 rate = 48000;
static u32 channels = 2;

static u32 *corb;                 /* commands out */
static u64 *rirb;                 /* responses back */
static u32  corb_wp;
static u32  rirb_rp;

static u8   codec_addr;
static u8   afg_nid;              /* the audio function group */
static u8   dac_nid, pin_nid;
static char described[96];

/* Sixteen byte entries: where, how much, and whether to interrupt after. */
typedef struct {
    u64 addr;
    u32 len;
    u32 flags;
} __attribute__((packed)) bdl_entry_t;

static bdl_entry_t *bdl;

static u16 r16(u32 off)         { return *(volatile u16 *)(regs + off); }
static u32 r32(u32 off)         { return *(volatile u32 *)(regs + off); }
static void w8(u32 off, u8 v)   { *(volatile u8 *)(regs + off) = v; }
static void w16(u32 off, u16 v) { *(volatile u16 *)(regs + off) = v; }
static void w32(u32 off, u32 v) { *(volatile u32 *)(regs + off) = v; }

static u32 sd_off(u32 reg) { return SD_BASE + out_stream * SD_STRIDE + reg; }

static void *alloc_aligned(u64 bytes, u64 align) {
    u8 *raw = (u8 *)kmalloc(bytes + align);
    if (!raw) return 0;
    u64 addr = ((u64)raw + align - 1) & ~(align - 1);
    memset((void *)addr, 0, bytes);
    return (void *)addr;
}

/* Waiting, in microseconds, without a timer fine enough to do it properly.
   A write to a port nobody answers takes about one. */
static void spin_us(u32 us) {
    while (us--) io_wait();
}

/* --- talking to the codec ------------------------------------------------ */

/* One command, and the answer to it.
 *
 * The rings are one deep as far as this is concerned: a command is posted,
 * the response ring's write pointer is watched until it moves, and the entry
 * it moved to is the answer. Nothing else is ever in flight, which costs
 * some setup time at boot and removes every question about which response
 * belongs to which command. */
/* For the self test: a codec simulated in software, answering in place of
   the link (hda_test_route). */
static bool (*test_codec)(u8 nid, u32 payload, u32 *out);

static bool codec(u8 nid, u32 payload, u32 *out) {
    if (test_codec) return test_codec(nid, payload, out);
    if (!corb || !rirb) return false;

    u32 verb = ((u32)codec_addr << 28) | ((u32)nid << 20) | (payload & 0xFFFFF);

    corb_wp = (corb_wp + 1) % RING_ENTRIES;
    corb[corb_wp] = verb;
    w16(CORBWP, (u16)corb_wp);

    for (u32 i = 0; i < 20000; i++) {
        u32 wp = r16(RIRBWP) % RING_ENTRIES;
        if (wp != rirb_rp) {
            rirb_rp = (rirb_rp + 1) % RING_ENTRIES;
            u64 entry = rirb[rirb_rp];
            w8(RIRBSTS, 0x05);          /* acknowledge, so it keeps going */
            if (out) *out = (u32)entry;
            return true;
        }
        spin_us(10);
    }
    return false;
}

static u32 param(u8 nid, u8 p) {
    u32 v = 0;
    codec(nid, GET_PARAM(p), &v);
    return v;
}

static void tell(u8 nid, u32 payload) {
    codec(nid, payload, 0);
}

/* --- finding a way out --------------------------------------------------- */

static u8 widget_type(u8 nid) {
    return (u8)((param(nid, P_WIDGET_CAPS) >> 20) & 0x0F);
}

/* The gain an amplifier calls nought decibels, held to the steps it has.
 *
 * Every amplifier was asked for 0x7F, the most the field holds, on the
 * understanding that each would stop at its own top step. The specification
 * leaves a gain past the last step undefined, and the codecs in machines
 * put nought decibels well below the top, where 0x7F is past it or a boost.
 * QEMU's scales the gain to a byte for its mixer, so 0x7F against its 0x4A
 * steps wrapped round to about seven tenths of full volume. An amplifier
 * that describes nothing is asked for the most, as before. */
static u32 amp_unity(u8 nid, bool out) {
    u8 p = out ? P_OUT_AMP_CAP : P_IN_AMP_CAP;
    bool own = (param(nid, P_WIDGET_CAPS) & WCAP_AMP_OVERRIDE) != 0;
    u32 cap = param(own ? nid : afg_nid, p);
    if (!cap) return 0x7F;
    u32 unity = cap & 0x7F, steps = (cap >> 8) & 0x7F;
    return unity > steps ? steps : unity;
}

/* Whether this pin is one a sound could come out of, and how much it looks
   like the one that was meant to.
 *
 * Higher is better. A pin the manufacturer described as the internal speaker
 * beats a line out, which beats a socket the firmware says has nothing wired
 * to it at all. Without this a laptop plays into whichever socket happened to
 * be enumerated first, which is often the one on the back nobody uses. */
static int pin_score(u8 nid) {
    if (widget_type(nid) != WIDGET_PIN) return -1;
    if (!(param(nid, P_PIN_CAPS) & PIN_CAP_OUTPUT)) return -1;

    u32 cfg = 0;
    codec(nid, GET_CONFIG_DEFAULT, &cfg);
    u32 connectivity = (cfg >> 30) & 0x3;
    u32 device = (cfg >> 20) & 0xF;

    if (connectivity == 1) return 0;      /* nothing is wired to it */

    if (device == 1) return 100;          /* the speaker */
    if (device == 0) return 90;           /* line out */
    if (device == 2) return 80;           /* headphones */
    return 10;
}

/* The widgets a widget's connection list names, as many as fit in out: an
 * answer to GET_CONN carries four of them in the short form and two in the
 * long, starting at the index asked for rounded down to that many.
 *
 * Only the lowest byte of each answer was read, so entry one, two or three
 * came back as entry nought again, and a pin or a mixer with more than one
 * input was looked behind through its first alone. QEMU's codec has one
 * input everywhere and never showed it; the codecs in machines put the
 * converter behind a mixer's second input as often as its first. A range
 * (the top bit of an entry) is taken as its last widget. */
static u32 conn_list(u8 nid, u8 *out, u32 cap) {
    u32 len = param(nid, P_CONN_LEN);
    bool wide = (len & 0x80) != 0;
    u32 count = len & 0x7F, per = wide ? 2 : 4, n = 0;
    for (u32 i = 0; i < count && n < cap; i += per) {
        u32 resp = 0;
        if (!codec(nid, GET_CONN(i), &resp)) break;
        for (u32 k = 0; k < per && i + k < count && n < cap; k++) {
            u32 e = wide ? (resp >> (16 * k)) & 0xFFFF : (resp >> (8 * k)) & 0xFF;
            out[n++] = (u8)(e & (wide ? 0x7FFF : 0x7F));
        }
    }
    return n;
}

/* A way back from the pin to a converter, through mixers and selectors, at
 * most three steps: route[0] is the pin, route[route_n - 1] the converter,
 * and through[j] which of route[j]'s inputs leads to route[j + 1]. Nearer
 * converters are looked for before deeper ones. */
#define ROUTE_MAX 5
static u8  route[ROUTE_MAX], through[ROUTE_MAX];
static u32 route_n;

static bool route_from(u8 nid, u32 depth) {
    u8 conns[16];
    u32 n = conn_list(nid, conns, 16);
    route[depth] = nid;
    for (u32 i = 0; i < n; i++)
        if (conns[i] && widget_type(conns[i]) == WIDGET_DAC) {
            through[depth] = (u8)i;
            route[depth + 1] = conns[i];
            route_n = depth + 2;
            return true;
        }
    if (depth + 2 >= ROUTE_MAX) return false;
    for (u32 i = 0; i < n; i++) {
        u8 t = conns[i] ? widget_type(conns[i]) : 0xFF;
        if (t != WIDGET_MIXER && t != WIDGET_SELECTOR) continue;
        through[depth] = (u8)i;
        if (route_from(conns[i], depth + 1)) return true;
    }
    return false;
}

/* Everything on the route awake and open: each widget's output amplifier
   unmuted, a pin or selector told which input to take, and a mixer's input
   from the next widget unmuted -- a mixer's inputs start muted on the codecs
   in machines, which is silence with every other register right. Each
   amplifier is set to its own nought decibels; how loud is the mixing's to
   say, in software. */
static void route_open(void) {
    for (u32 j = 0; j < route_n; j++) {
        u8 nid = route[j];
        tell(nid, SET_POWER(0));
        tell(nid, SET_AMP(AMP_OUT(amp_unity(nid, true))));
        if (j + 1 >= route_n) continue;
        u8 t = widget_type(nid);
        if (t == WIDGET_MIXER) tell(nid, SET_AMP(AMP_IN((u32)through[j], amp_unity(nid, false))));
        else tell(nid, SET_CONN_SEL(through[j]));
    }
}

/* The audio function group, and every widget under it. */
static bool find_route(void) {
    u32 sub = param(0, P_NODE_COUNT);
    u8 first_fg = (u8)((sub >> 16) & 0xFF);
    u8 n_fg = (u8)(sub & 0xFF);

    for (u8 fg = first_fg; fg < first_fg + n_fg; fg++) {
        if ((param(fg, P_FUNC_TYPE) & 0xFF) != 1) continue;   /* not audio */

        afg_nid = fg;
        tell(fg, SET_POWER(0));                               /* D0, awake */
        spin_us(1000);

        u32 wsub = param(fg, P_NODE_COUNT);
        u8 first_w = (u8)((wsub >> 16) & 0xFF);
        u8 n_w = (u8)(wsub & 0xFF);
        if (!n_w || n_w > MAX_WIDGETS) n_w = MAX_WIDGETS;

        int best = 0;
        u8 best_pin = 0;
        for (u8 w = first_w; w < first_w + n_w; w++) {
            int s = pin_score(w);
            if (s > best) { best = s; best_pin = w; }
        }
        if (!best_pin) continue;

        u8 dac = 0;
        route_n = 0;
        if (route_from(best_pin, 0)) dac = route[route_n - 1];
        else {
            /* No connection list worth following. Some codecs wire a single
               converter straight to the pin and do not describe it, so take
               the first converter there is. (QEMU's does describe it: its
               pin 3 names converter 2.) */
            for (u8 w = first_w; w < first_w + n_w; w++)
                if (widget_type(w) == WIDGET_DAC) { dac = w; break; }
            route[0] = best_pin;
            route[1] = dac;
            route_n = 2;
        }
        if (!dac) continue;

        pin_nid = best_pin;
        dac_nid = dac;

        route_open();
        tell(pin_nid, SET_PIN_CTL(PIN_OUT_ENABLE));
        tell(pin_nid, SET_EAPD(0x02));      /* laptops need this or silence */
        return true;
    }
    return false;
}

/* --- setting up ---------------------------------------------------------- */

static bool reset_controller(void) {
    w32(GCTL, r32(GCTL) & ~GCTL_RESET);
    for (u32 i = 0; i < 1000; i++) {
        if (!(r32(GCTL) & GCTL_RESET)) break;
        spin_us(100);
    }
    spin_us(1000);

    w32(GCTL, r32(GCTL) | GCTL_RESET);
    for (u32 i = 0; i < 1000; i++) {
        if (r32(GCTL) & GCTL_RESET) break;
        spin_us(100);
    }
    if (!(r32(GCTL) & GCTL_RESET)) return false;

    /* The codecs need time to notice the reset and say they are there. The
       specification asks for at least 521 microseconds and says nothing
       about what happens if you look sooner, which is that you find no
       codecs and decide the machine has no sound. */
    spin_us(2000);
    return true;
}

static bool start_rings(void) {
    corb = (u32 *)alloc_aligned(RING_ENTRIES * 4, 128);
    rirb = (u64 *)alloc_aligned(RING_ENTRIES * 8, 128);
    if (!corb || !rirb) return false;

    w8(CORBCTL, 0);
    w8(RIRBCTL, 0);

    w8(CORBSIZE, 0x02);                    /* 256 entries */
    w32(CORBLBASE, (u32)(u64)corb);
    w32(CORBUBASE, (u32)((u64)corb >> 32));

    /* Reset the read pointer, which has to be asked for and then watched:
       the bit is written, read back set, cleared, and read back clear. */
    w16(CORBRP, CORBRP_RST);
    for (u32 i = 0; i < 1000 && !(r16(CORBRP) & CORBRP_RST); i++) spin_us(10);
    w16(CORBRP, 0);
    for (u32 i = 0; i < 1000 && (r16(CORBRP) & CORBRP_RST); i++) spin_us(10);

    w16(CORBWP, 0);
    corb_wp = 0;

    w8(RIRBSIZE, 0x02);
    w32(RIRBLBASE, (u32)(u64)rirb);
    w32(RIRBUBASE, (u32)((u64)rirb >> 32));
    w16(RIRBWP, (u16)(1u << 15));          /* reset its write pointer */
    w16(RINTCNT, 1);
    rirb_rp = 0;

    w8(CORBCTL, CORBCTL_RUN);
    w8(RIRBCTL, RIRBCTL_RUN | RIRBCTL_IRQ);
    return true;
}

bool hda_init(void) {
    present = false;

    pci_dev_t dev;
    /* Class 4 subclass 3 is HD Audio, and the interface byte is zero on every
       controller that is one. AC'97 is subclass 1 and is not this. */
    if (!pci_find_class(0x04, 0x03, 0x00, &dev)) { bb_log("hda: no pci class 4/3/0"); return false; }

    u64 bar = dev.bar0 & 0xFFFFFFF0u;
    /* A memory window is allowed to be sixty four bits wide, in which case
       the top half is in the next register. Real controllers often are;
       emulated ones usually are not. */
    if (((dev.bar0 >> 1) & 0x3) == 0x2) {
        u32 high = pci_read32(dev.bus, dev.slot, dev.func, 0x14);
        bar |= (u64)high << 32;
    }
    if (!bar) { bb_log("hda: bar0 empty"); return false; }

    regs = (volatile u8 *)paging_map_device(bar, 0x4000);
    if (!regs) { bb_log("hda: could not map %x", (u32)bar); return false; }

    pci_enable_bus_master(&dev);

    if (!reset_controller()) { bb_log("hda: controller would not reset, gctl %x", r32(GCTL)); return false; }

    /* Output stream descriptors come after the input ones, and how many of
       each there are is the controller's to say. */
    u16 cap = r16(GCAP);
    u32 inputs = (cap >> 8) & 0x0F;
    u32 outputs = (cap >> 12) & 0x0F;
    if (!outputs) { bb_log("hda: gcap %x says no output streams", (u32)cap); return false; }
    out_stream = inputs;

    if (!start_rings()) { bb_log("hda: rings would not start"); return false; }

    /* Whichever codec answered. There is usually one. */
    u16 answered = r16(STATESTS);
    if (!answered) { bb_log("hda: no codec answered, statests 0"); return false; }
    for (u8 i = 0; i < 15; i++)
        if (answered & (1u << i)) { codec_addr = i; break; }

    u32 vendor = param(0, P_VENDOR);
    if (!find_route()) { bb_log("hda: codec %d vendor %x, no route out", (u32)codec_addr, vendor); return false; }

    present = true;
    kformat(described, sizeof described,
            "hda codec %x:%x, converter %d into pin %d",
            (u32)(vendor >> 16), (u32)(vendor & 0xFFFF),
            (u32)dac_nid, (u32)pin_nid);
    /* The whole way, pin first: on a machine with no sound, what was chosen
       is the first thing to know. */
    char way[40];
    u32 at = 0;
    way[0] = 0;
    for (u32 j = 0; j < route_n && at + 6 < sizeof way; j++)
        at += (u32)kformat(way + at, sizeof way - at, j ? " %d" : "%d", (u32)route[j]);
    bb_log("audio %s, by way of %s", described, way);
    return true;
}

bool hda_present(void)     { return present; }
const char *hda_describe(void) { return present ? described : "none"; }
u32  hda_rate(void)        { return rate; }
u32  hda_channels(void)    { return channels; }
u32  hda_frame_bytes(void) { return channels * 2; }

bool hda_start(void *buffer, u32 bytes, u32 rate_hz) {
    if (!present || !buffer || !bytes) return false;
    rate = rate_hz;

    /* Four descriptors over the one buffer. The specification wants at least
       two, and more of them only means the controller reports its progress
       more often, which nothing here reads. */
    bdl = (bdl_entry_t *)alloc_aligned(4 * sizeof(bdl_entry_t), 128);
    if (!bdl) return false;

    u32 quarter = bytes / 4;
    for (u32 i = 0; i < 4; i++) {
        bdl[i].addr = (u64)buffer + i * quarter;
        bdl[i].len = quarter;
        bdl[i].flags = 0;
    }

    /* Stopped, and reset, before any of it is described. A stream descriptor
       written while the stream is running is a stream that keeps playing
       whatever it was playing. */
    w32(sd_off(SD_CTL), 0);
    for (u32 i = 0; i < 1000 && (r32(sd_off(SD_CTL)) & SD_CTL_RUN); i++)
        spin_us(10);

    w32(sd_off(SD_CTL), SD_CTL_SRST);
    for (u32 i = 0; i < 1000 && !(r32(sd_off(SD_CTL)) & SD_CTL_SRST); i++)
        spin_us(10);
    w32(sd_off(SD_CTL), 0);
    for (u32 i = 0; i < 1000 && (r32(sd_off(SD_CTL)) & SD_CTL_SRST); i++)
        spin_us(10);

    w8(sd_off(SD_STS), 0x1C);                  /* clear what happened before */
    w32(sd_off(SD_CBL), quarter * 4);
    w16(sd_off(SD_LVI), 3);
    w32(sd_off(SD_BDPL), (u32)(u64)bdl);
    w32(sd_off(SD_BDPU), (u32)((u64)bdl >> 32));

    /* 48 kHz, sixteen bits, two channels. The rate field is a base and two
       divisors rather than a number, and 48 kHz is the base with neither. */
    u16 fmt = (u16)(((channels - 1) & 0x0F) | (1u << 4));
    w16(sd_off(SD_FMT), fmt);

    /* Stream one. The number has to match on both sides: the controller tags
       the bytes with it and the converter is told to listen for that tag, and
       nothing complains if they disagree, there is simply no sound. */
    tell(dac_nid, SET_FORMAT(fmt));
    tell(dac_nid, SET_STREAM_CHAN(0x10));      /* stream 1, channel 0 */

    w32(sd_off(SD_CTL), (1u << 20) | SD_CTL_RUN);
    return true;
}

u32 hda_position(void) {
    if (!present) return 0;
    return r32(sd_off(SD_LPIB));
}

/* --- for the self test ------------------------------------------------------ */

bool hda_test_route(bool (*fn)(u8 nid, u32 payload, u32 *out), u8 *pin, u8 *dac) {
    bool (*was)(u8, u32, u32 *) = test_codec;
    u8 was_addr = codec_addr, was_afg = afg_nid, was_pin = pin_nid, was_dac = dac_nid;
    test_codec = fn;
    codec_addr = 0;
    pin_nid = dac_nid = 0;
    bool found = find_route();
    *pin = pin_nid;
    *dac = dac_nid;
    test_codec = was;
    codec_addr = was_addr;
    afg_nid = was_afg;
    pin_nid = was_pin;
    dac_nid = was_dac;
    return found;
}
