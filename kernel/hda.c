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
#define P_CONN_LEN      0x0E

#define WIDGET_DAC      0
#define WIDGET_MIXER    2
#define WIDGET_SELECTOR 3
#define WIDGET_PIN      4

#define PIN_CAP_OUTPUT  (1u << 4)

/* Turn the amplifier on: output side, both channels, no mute, full gain.
   The gain field is seven bits and what full means differs per widget, so
   the widget's own capability would be the careful thing to read. This asks
   for more than any of them have, and they clamp. */
#define AMP_OUT_UNMUTE  0xB07F

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
static bool codec(u8 nid, u32 payload, u32 *out) {
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

/* Walks back from a pin looking for a converter.
 *
 * One hop through a mixer or a selector is allowed, which covers nearly
 * everything: pins rarely hang off a converter directly, and the thing in
 * between is usually a single mixer. When the widget in the way is a
 * selector it also has to be told which of its inputs to listen to. */
static u8 dac_behind(u8 pin) {
    u32 count = param(pin, P_CONN_LEN) & 0x7F;
    if (!count) return 0;

    for (u32 i = 0; i < count && i < 8; i++) {
        u32 list = 0;
        codec(pin, GET_CONN(i), &list);
        u8 first = (u8)(list & 0xFF);
        if (!first) continue;

        if (widget_type(first) == WIDGET_DAC) return first;

        u8 mid = widget_type(first);
        if (mid == WIDGET_MIXER || mid == WIDGET_SELECTOR) {
            u32 inner = param(first, P_CONN_LEN) & 0x7F;
            for (u32 j = 0; j < inner && j < 8; j++) {
                u32 sub = 0;
                codec(first, GET_CONN(j), &sub);
                u8 cand = (u8)(sub & 0xFF);
                if (cand && widget_type(cand) == WIDGET_DAC) {
                    if (mid == WIDGET_SELECTOR) tell(first, SET_CONN_SEL(j));
                    tell(first, SET_POWER(0));
                    tell(first, SET_AMP(AMP_OUT_UNMUTE));
                    return cand;
                }
            }
        }
    }
    return 0;
}

/* The audio function group, and every widget under it. */
static bool find_route(void) {
    u32 sub = param(0, P_NODE_COUNT);
    u8 first_fg = (u8)((sub >> 16) & 0xFF);
    u8 n_fg = (u8)(sub & 0xFF);

    for (u8 fg = first_fg; fg < first_fg + n_fg; fg++) {
        if ((param(fg, P_FUNC_TYPE) & 0xFF) != 1) continue;   /* not audio */

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

        u8 dac = dac_behind(best_pin);
        if (!dac) {
            /* No connection list worth following. Some codecs, including the
               simple one QEMU presents, wire a single converter straight to
               the pin and do not describe it, so take the first converter
               there is. */
            for (u8 w = first_w; w < first_w + n_w; w++)
                if (widget_type(w) == WIDGET_DAC) { dac = w; break; }
        }
        if (!dac) continue;

        pin_nid = best_pin;
        dac_nid = dac;

        tell(pin_nid, SET_POWER(0));
        tell(dac_nid, SET_POWER(0));
        tell(pin_nid, SET_PIN_CTL(PIN_OUT_ENABLE));
        tell(pin_nid, SET_EAPD(0x02));      /* laptops need this or silence */
        tell(pin_nid, SET_AMP(AMP_OUT_UNMUTE));
        tell(dac_nid, SET_AMP(AMP_OUT_UNMUTE));
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
    bb_log("audio %s", described);
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
