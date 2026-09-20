/* Ensoniq AudioPCI, both of them.
 *
 * The ES1370 and the ES1371 are one design with the front end swapped, and
 * the half that matters is the half they share. A block of memory, a size, a
 * format and an enable bit: the chip reads that block round and round and
 * whatever is in it comes out of the speaker. There is no starting a sound
 * and no stopping one, which is exactly the arrangement kernel/sound.c above
 * already has for HD Audio, so the two fit behind the same four calls.
 *
 * What differs is everything in front of the converter.
 *
 *   The ES1370 makes its rate by dividing a 1.4112 MHz clock, so it can play
 *   at 44100 exactly and at 22050 and 11025 and not at anything else. Its
 *   mixer is an AK4531 reached through a single write port.
 *
 *   The ES1371 puts a sample rate converter in front of the same engine and
 *   an AC97 codec behind it. The converter can play any rate and has to be
 *   programmed, and the manual is emphatic that switching it on with its
 *   parameters unset locks the chip until the power goes off. So the order
 *   below is: fill every one of its registers, then enable it.
 *
 * Which one is here is decided by the device id and then never asked again:
 * every difference is settled at startup and the playing path is common.
 *
 * Why this exists at all: VMware hands a guest it does not recognise an
 * ES1371 unless somebody edits the machine's configuration file, and it does
 * not recognise a system written from scratch. A kernel that drives only HD
 * Audio has working sound on QEMU and silence on the way most people will
 * actually run it.
 */
#include "ens.h"
#include "pci.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "blackbox.h"

#define VENDOR_ENSONIQ 0x1274
#define DEV_ES1370     0x5000
#define DEV_ES1371     0x1371
#define DEV_CT5880     0x5880
#define DEV_ES1373_A   0x8001
#define DEV_ES1373_B   0x8002

/* --- registers, as offsets from the I/O window --------------------------- */

#define REG_CONTROL     0x00
#define REG_STATUS      0x04
#define REG_MEM_PAGE    0x0C
#define REG_1370_CODEC  0x10      /* the AK4531, one write port */
#define REG_1371_SRC    0x10      /* the sample rate converter, same address */
#define REG_1371_CODEC  0x14      /* AC97 */
#define REG_LEGACY      0x18
#define REG_SERIAL      0x20
#define REG_DAC2_COUNT  0x28

/* The last sixteen bytes of the window are a movable pane. Which registers
   appear there is chosen by the page register, and page twelve is where the
   playback channels keep their addresses. */
#define PAGE_DAC        0x0C
#define REG_DAC2_FRAME  0x38
#define REG_DAC2_SIZE   0x3C

/* CONTROL */
#define CTRL_PCLKDIV(o)   (((o) & 0x1FFFu) << 16)
#define CTRL_PCLKDIV_MASK (0x1FFFu << 16)
#define CTRL_1371_SYNC_RES (1u << 14)
#define CTRL_DAC1_EN      (1u << 6)
#define CTRL_DAC2_EN      (1u << 5)
#define CTRL_ADC_EN       (1u << 4)
#define CTRL_UART_EN      (1u << 3)
#define CTRL_JYSTK_EN     (1u << 2)
#define CTRL_1370_CDC_EN  (1u << 1)
#define CTRL_1371_XTAL_EN (1u << 1)
#define CTRL_SERR_DIS     (1u << 0)

/* SERIAL. The three format fields are two bits each: the low bit says
   stereo, the high one says sixteen bit. */
#define SER_P2_END_INC(o) (((o) & 7u) << 19)
#define SER_P2_ST_INC(o)  (((o) & 7u) << 16)
#define SER_P2_LOOP_SEL   (1u << 14)
#define SER_P2_PAUSE      (1u << 12)
#define SER_P2_INT_EN     (1u << 9)
#define SER_P2_DAC_SEN    (1u << 6)
#define SER_P2_FMT(o)     (((o) & 3u) << 2)
#define FMT_16_STEREO     3u

/* The ES1371's AC97 port */
#define CODEC_RDY   (1u << 31)
#define CODEC_WIP   (1u << 30)
#define CODEC_PIRD  (1u << 23)

/* The ES1371's sample rate converter */
#define SRC_ADDR(o)   (((o) & 0x7Fu) << 25)
#define SRC_WE        (1u << 24)
#define SRC_BUSY      (1u << 23)
#define SRC_DISABLE   (1u << 22)
#define SRC_DIS_P1    (1u << 21)
#define SRC_DIS_P2    (1u << 20)
#define SRC_DIS_R1    (1u << 19)
#define SRC_DATA(o)   ((o) & 0xFFFFu)

/* Its register file, sixteen bits a word. */
#define SMP_DAC1        0x70
#define SMP_DAC2        0x74
#define SMP_TRUNC_N     0x00
#define SMP_INT_REGS    0x01
#define SMP_VFREQ_FRAC  0x03
#define SMP_VOL_ADC     0x6C
#define SMP_VOL_DAC1    0x7C
#define SMP_VOL_DAC2    0x7E

#define POLL 100000u

static pci_dev_t dev;
static u16  io_base;
static bool present;
static bool is_1371;
static u32  rate = 44100;
static u32  channels = 2;
static char described[80];

bool ens_present(void)    { return present; }
u32  ens_rate(void)       { return rate; }
u32  ens_channels(void)   { return channels; }
u32  ens_frame_bytes(void){ return channels * 2; }
const char *ens_describe(void) { return present ? described : "none"; }

static u32 ctl_shadow;
static u32 ser_shadow;

static void reg_write(u16 reg, u32 v) { outl((u16)(io_base + reg), v); }
static u32  reg_read(u16 reg)         { return inl((u16)(io_base + reg)); }

/* A delay measured on the one clock that exists before any driver has set
   anything up. Bit four of port 0x61 toggles every 15.085 microseconds on
   every PC there has ever been, and unlike the timer it does not need
   interrupts, which are still off while this runs. */
static void udelay(u32 us) {
    for (u32 i = 0; i < us / 15 + 1; i++) {
        u8 start = inb(0x61) & 0x10;
        u32 guard = 0;
        while ((inb(0x61) & 0x10) == start && guard++ < 100000) { }
    }
}

/* --- the ES1371's front end --------------------------------------------- */

static u32 src_wait(void) {
    u32 r = 0;
    for (u32 t = 0; t < POLL; t++) {
        r = reg_read(REG_1371_SRC);
        if (!(r & SRC_BUSY)) return r;
    }
    return r;
}

/* The converter's register file is written through the same port that
   controls it, so the bits that say which channels are switched off have to
   be carried through every write or the write turns them back on. */
static void src_write(u16 reg, u16 data) {
    u32 r = src_wait() & (SRC_DISABLE | SRC_DIS_P1 | SRC_DIS_P2 | SRC_DIS_R1);
    r |= SRC_ADDR(reg) | SRC_DATA(data);
    reg_write(REG_1371_SRC, r | SRC_WE);
}

static u16 src_read(u16 reg) {
    u32 r = src_wait() & (SRC_DISABLE | SRC_DIS_P1 | SRC_DIS_P2 | SRC_DIS_R1);
    reg_write(REG_1371_SRC, r | SRC_ADDR(reg));
    return (u16)(src_wait() & 0xFFFF);
}

/* The playback rate, as the converter wants it: the ratio against the 3 kHz
   the accumulator counts in, in fifteen bits of fraction. */
static void src_dac2_rate(u32 hz) {
    u32 freq = ((hz << 15) + 1500u) / 3000u;

    u32 r = src_wait() & (SRC_DISABLE | SRC_DIS_P1 | SRC_DIS_R1);
    reg_write(REG_1371_SRC, r | SRC_DIS_P2);        /* stopped while it moves */

    u16 ints = src_read(SMP_DAC2 + SMP_INT_REGS);
    src_write(SMP_DAC2 + SMP_INT_REGS,
              (u16)((ints & 0x00FF) | ((freq >> 5) & 0xFC00)));
    src_write(SMP_DAC2 + SMP_VFREQ_FRAC, (u16)(freq & 0x7FFF));

    r = src_wait() & (SRC_DISABLE | SRC_DIS_P1 | SRC_DIS_R1);
    reg_write(REG_1371_SRC, r);
}

/* The other playback channel's rate. Nothing here plays through it, and it
   is set anyway: the converter is one machine with three channels sharing an
   accumulator, and the part it is least forgiving about is being switched on
   with any of them holding nonsense. */
static void src_dac1_rate(u32 hz) {
    u32 freq = ((hz << 15) + 1500u) / 3000u;

    u32 r = src_wait() & (SRC_DISABLE | SRC_DIS_P2 | SRC_DIS_R1);
    reg_write(REG_1371_SRC, r | SRC_DIS_P1);

    u16 ints = src_read(SMP_DAC1 + SMP_INT_REGS);
    src_write(SMP_DAC1 + SMP_INT_REGS,
              (u16)((ints & 0x00FF) | ((freq >> 5) & 0xFC00)));
    src_write(SMP_DAC1 + SMP_VFREQ_FRAC, (u16)(freq & 0x7FFF));

    r = src_wait() & (SRC_DISABLE | SRC_DIS_P2 | SRC_DIS_R1);
    reg_write(REG_1371_SRC, r);
}

/* Every word of it, before it is allowed to run.
 *
 * The order is not a style choice. The part refuses to come back from being
 * enabled with rubbish in its accumulators: the busy bit sticks high and
 * nothing short of power clears it. So the file is zeroed, the four numbers
 * that have to be right are written, and only then is the disable bit
 * dropped. */
static void src_init(void) {
    src_wait();
    reg_write(REG_1371_SRC, SRC_DISABLE);

    for (u16 i = 0; i < 0x80; i++) src_write(i, 0);

    src_write(SMP_DAC1 + SMP_TRUNC_N,  16 << 4);
    src_write(SMP_DAC1 + SMP_INT_REGS, 16 << 10);
    src_write(SMP_DAC2 + SMP_TRUNC_N,  16 << 4);
    src_write(SMP_DAC2 + SMP_INT_REGS, 16 << 10);

    /* Unity through the converter. Its own volume is not the mixer's, and a
       zero here is silence with every mixer register set correctly, which is
       an unpleasant thing to look for. */
    src_write(SMP_VOL_ADC,      1 << 12);
    src_write(SMP_VOL_ADC + 1,  1 << 12);
    src_write(SMP_VOL_DAC1,     1 << 12);
    src_write(SMP_VOL_DAC1 + 1, 1 << 12);
    src_write(SMP_VOL_DAC2,     1 << 12);
    src_write(SMP_VOL_DAC2 + 1, 1 << 12);

    src_dac1_rate(22050);
    src_dac2_rate(rate);

    src_wait();
    reg_write(REG_1371_SRC, 0);
}

/* AC97, through a port that is busy for a while after each write and that
   must only be written at a point in the converter's cycle where the
   converter is not itself using the link.
 *
 * The two short waits are that point: past whatever state the converter was
 * in, and then into the first one, which is where the window is. They are
 * bounded far more tightly than the wait for the port itself, because on
 * hardware they are satisfied within a few microseconds and on something
 * that does not model the converter's state at all they are never satisfied
 * at all -- and five registers times a hundred thousand port reads, each one
 * a trip out of the virtual machine, is a second of boot spent waiting for
 * something that is not coming. Missing the window costs a mixer register;
 * spinning for it costs the boot. */
#define CODEC_SETTLE 4000u

static void ac97_write(u16 reg, u16 val) {
    for (u32 t = 0; t < POLL; t++) {
        if (reg_read(REG_1371_CODEC) & CODEC_WIP) continue;

        for (u32 i = 0; i < CODEC_SETTLE; i++)
            if ((reg_read(REG_1371_SRC) & 0x00870000u) == 0) break;
        for (u32 i = 0; i < CODEC_SETTLE; i++)
            if ((reg_read(REG_1371_SRC) & 0x00870000u) == 0x00010000u) break;

        reg_write(REG_1371_CODEC, ((u32)(reg & 0x7F) << 16) | val);
        return;
    }
}

/* --- the ES1370's front end ---------------------------------------------- */

/* The AK4531 takes a register and a value in one sixteen bit write and is
   busy afterwards. Nothing reads back from it. */
static void ak4531_write(u8 reg, u8 val) {
    for (u32 t = 0; t < POLL; t++)
        if (!(reg_read(REG_STATUS) & (1u << 8))) break;      /* CSTAT */
    outw((u16)(io_base + REG_1370_CODEC), (u16)(((u16)reg << 8) | val));
    udelay(100);
}

/* The rate is a divider off 1.4112 MHz, so it is not free: 44100 comes out
   exactly and 48000 does not come out at all. Reported rather than assumed,
   which is why sound.c asks what it got. */
#define DIV_FOR(hz) ((1411200u + (hz) / 2) / (hz) - 2)

/* --- the part they share ------------------------------------------------- */

bool ens_start(void *buffer, u32 bytes, u32 rate_hz) {
    if (!present || !buffer || bytes < 64) return false;

    u64 phys = (u64)buffer;
    if (phys >> 32) return false;                  /* the chip has 32 bits */
    if (phys & 3) return false;                    /* and reads whole words */

    if (is_1371) {
        rate = rate_hz;
        src_dac2_rate(rate);
    } else {
        /* The older half has no converter. Its second playback channel is
           specified from 5512 Hz to 44100 and the divider will happily
           produce numbers outside that on either side, so the request is
           held to the range first and then turned into the nearest divider.

           Asking for 48000 and being given 48662 is what happens without
           this, and it is not merely out of specification: everything that
           makes a sound reads the rate back and generates for it, so the
           note is correct against a rate the part was never meant to run
           at, and how far out it lands is up to whatever is downstream. */
        u32 want = rate_hz;
        if (want > 44100) want = 44100;
        if (want < 5512)  want = 5512;

        u32 div = DIV_FOR(want);
        if (div > 0x1FFF) div = 0x1FFF;
        rate = 1411200u / (div + 2);
        ctl_shadow = (ctl_shadow & ~CTRL_PCLKDIV_MASK) | CTRL_PCLKDIV(div);
    }

    u32 frames = bytes / ens_frame_bytes();
    u32 count = frames ? frames - 1 : 0;
    if (count > 0xFFFF) count = 0xFFFF;

    /* Stopped while it is pointed somewhere else. */
    ctl_shadow &= ~CTRL_DAC2_EN;
    reg_write(REG_CONTROL, ctl_shadow);

    reg_write(REG_MEM_PAGE, PAGE_DAC);
    reg_write(REG_DAC2_FRAME, (u32)phys);
    /* The size is in whole words, and one less than there are: the chip
       counts up to it rather than to the end. */
    reg_write(REG_DAC2_SIZE, (bytes / 4) - 1);
    reg_write(REG_DAC2_COUNT, (count << 16) | count);

    /* Sixteen bit stereo, no interrupt, not paused, and looping -- the loop
       select bit means stop at the end, which is the one thing a buffer that
       is meant to play forever must not do. */
    ser_shadow &= ~(SER_P2_END_INC(7) | SER_P2_ST_INC(7) | SER_P2_LOOP_SEL |
                    SER_P2_PAUSE | SER_P2_INT_EN | SER_P2_DAC_SEN |
                    SER_P2_FMT(3));
    ser_shadow |= SER_P2_END_INC(2) | SER_P2_ST_INC(0) |
                  SER_P2_FMT(FMT_16_STEREO);
    reg_write(REG_SERIAL, ser_shadow);

    ctl_shadow |= CTRL_DAC2_EN;
    reg_write(REG_CONTROL, ctl_shadow);

    /* What the chip actually holds, read back rather than assumed. A
       register that did not take is the difference between a controller
       that is playing and one that looks exactly like it is. */
    reg_write(REG_MEM_PAGE, PAGE_DAC);
    bb_log("ens: ctl %x ser %x frame %x size %x count %x src %x",
           reg_read(REG_CONTROL), reg_read(REG_SERIAL),
           reg_read(REG_DAC2_FRAME), reg_read(REG_DAC2_SIZE),
           reg_read(REG_DAC2_COUNT), reg_read(REG_1371_SRC));

    kformat(described, sizeof described, "%s, dac2 16 bit stereo",
            is_1371 ? "ensoniq es1371" : "ensoniq es1370");
    return true;
}

u32 ens_position(void) {
    if (!present) return 0;
    /* The current word, in the top half of the size register. The page has
       to be selected first: the last four registers of the window are a
       pane onto whichever page was last chosen, and anything else that
       touches the chip may have moved it. */
    reg_write(REG_MEM_PAGE, PAGE_DAC);
    u32 v = reg_read(REG_DAC2_SIZE);
    return ((v >> 16) & 0xFFFFu) * 4u;
}

static bool find_card(void) {
    static const u16 IDS_1371[] = { DEV_ES1371, DEV_CT5880,
                                    DEV_ES1373_A, DEV_ES1373_B };

    for (u32 i = 0; i < sizeof IDS_1371 / sizeof IDS_1371[0]; i++)
        if (pci_find(VENDOR_ENSONIQ, IDS_1371[i], &dev)) {
            is_1371 = true;
            return true;
        }

    if (pci_find(VENDOR_ENSONIQ, DEV_ES1370, &dev)) {
        is_1371 = false;
        return true;
    }
    return false;
}

bool ens_init(void) {
    present = false;
    channels = 2;

    if (!find_card()) { bb_log("ens: no ensoniq audiopci on the bus"); return false; }

    pci_enable_bus_master(&dev);
    io_base = (u16)(dev.bar0 & ~0x3u);
    if (!io_base) { bb_log("ens: no i/o window"); return false; }

    /* Everything off, and nothing left running from whatever booted before
       this. The legacy window is switched off outright: it is a set of
       addresses the chip answers on to pretend to be a Sound Blaster, and
       they collide with real devices. */
    ctl_shadow = 0;
    ser_shadow = 0;
    reg_write(REG_CONTROL, 0);
    reg_write(REG_SERIAL, 0);
    reg_write(REG_LEGACY, 0);

    if (is_1371) {
        /* The crystal on, the PCI clock left alone, and a warm reset of the
           link to the codec so that it starts from a state this knows. */
        ctl_shadow = 0;
        reg_write(REG_CONTROL, ctl_shadow);
        udelay(20);
        reg_write(REG_CONTROL, ctl_shadow | CTRL_1371_SYNC_RES);
        udelay(20);
        reg_write(REG_CONTROL, ctl_shadow);
        udelay(500);

        /* The codec says when it is ready, and a codec that never does is a
           controller that will play silence very convincingly. It is not a
           reason to give up: QEMU's part of this family has no codec behind
           it at all and plays perfectly well. */
        bool ready = false;
        for (u32 t = 0; t < POLL; t++)
            if (reg_read(REG_1371_CODEC) & CODEC_RDY) { ready = true; break; }

        src_init();

        /* The mixer is set up whether or not the codec said it was ready.
           A codec that is not there ignores the writes and costs nothing;
           one that is there and never raised the bit -- which is what an
           emulated part that does not model the bit looks like -- is still
           a codec, and leaving it alone means leaving it however it was
           found. Muted is a perfectly ordinary way to find one, and silence
           from a controller that is otherwise working is the single hardest
           fault in this area to tell apart from every other kind. */
        ac97_write(0x00, 0);                /* reset to defaults */
        udelay(100);
        ac97_write(0x26, 0x0000);           /* out of power down */
        ac97_write(0x02, 0x0000);           /* master, loudest, not muted */
        ac97_write(0x04, 0x0000);           /* headphone, the same */
        ac97_write(0x06, 0x0000);           /* and mono out */
        ac97_write(0x18, 0x0808);           /* pcm out at unity */

        bb_log("ens: es1371 at %04x, codec %s",
               io_base, ready ? "ready" : "did not report ready");
    } else {
        /* The serial link to the mixer has to be running before the mixer
           can be told anything. */
        ctl_shadow = CTRL_1370_CDC_EN | CTRL_SERR_DIS;
        reg_write(REG_CONTROL, ctl_shadow);
        udelay(100);

        ak4531_write(0x00, 0x00);           /* master left, loudest */
        ak4531_write(0x01, 0x00);           /* master right */
        ak4531_write(0x02, 0x00);           /* voice left */
        ak4531_write(0x03, 0x00);           /* voice right */
        ak4531_write(0x0E, 0x01);           /* mic gain off */
        ak4531_write(0x10, 0x03);           /* voice to both outputs */
        ak4531_write(0x11, 0x03);
        ak4531_write(0x16, 0x00);           /* reset the mixer state */
        bb_log("ens: es1370 at %04x", io_base);
    }

    kformat(described, sizeof described, "%s",
            is_1371 ? "ensoniq es1371" : "ensoniq es1370");
    present = true;
    return true;
}
