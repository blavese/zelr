/* See include/rtc.h.
 *
 * Two things about this chip catch everybody.
 *
 * It updates in place, once a second, and a read taken during that update
 * returns a mixture of the old time and the new one. At a second boundary
 * that can be 11:59:59 becoming 12:00:00 and being read as 11:00:00, which
 * is an hour wrong and looks like a working clock. There is a bit that says
 * an update is in progress, and the reliable procedure is to wait for it to
 * clear, read everything, read everything again, and accept the answer only
 * when two consecutive reads agree.
 *
 * And the values are usually BCD rather than binary: the byte 0x22 means 22,
 * not 34. A status bit says which, and firmware disagrees about it often
 * enough that it has to be read rather than assumed.
 */
#include "rtc.h"
#include "io.h"
#include "acpi.h"
#include "string.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define REG_SECOND   0x00
#define REG_MINUTE   0x02
#define REG_HOUR     0x04
#define REG_WEEKDAY  0x06
#define REG_DAY      0x07
#define REG_MONTH    0x08
#define REG_YEAR     0x09
#define REG_STATUS_A 0x0A
#define REG_STATUS_B 0x0B

#define STATUS_A_UPDATING 0x80
#define STATUS_B_24HOUR   0x02
#define STATUS_B_BINARY   0x04

static bool present;
static bool binary_mode;
static bool hour24;

bool rtc_present(void) { return present; }

static u8 cmos_read(u8 reg) {
    /* Bit 7 of the index port is the non-maskable interrupt disable. Leaving
       it set for the duration of the read is conventional and harmless; what
       is not harmless is writing it back as zero and enabling something the
       firmware wanted off. */
    outb(CMOS_INDEX, (u8)(0x80 | reg));
    io_wait();
    return inb(CMOS_DATA);
}

static bool updating(void) {
    return (cmos_read(REG_STATUS_A) & STATUS_A_UPDATING) != 0;
}

static u8 from_bcd(u8 v) { return (u8)((v & 0x0F) + ((v >> 4) * 10)); }

/* One complete read, taken while no update is in progress. */
static void sample(rtc_time_t *t) {
    t->second  = cmos_read(REG_SECOND);
    t->minute  = cmos_read(REG_MINUTE);
    t->hour    = cmos_read(REG_HOUR);
    t->weekday = cmos_read(REG_WEEKDAY);
    t->day     = cmos_read(REG_DAY);
    t->month   = cmos_read(REG_MONTH);
    t->year    = cmos_read(REG_YEAR);
}

static bool same(const rtc_time_t *a, const rtc_time_t *b) {
    return a->second == b->second && a->minute == b->minute &&
           a->hour == b->hour && a->day == b->day &&
           a->month == b->month && a->year == b->year;
}

bool rtc_init(void) {
    present = false;

    /* A machine with no clock reads as 0xFF everywhere, and one whose
       registers are all zero is not telling the time either. */
    u8 a = cmos_read(REG_STATUS_A);
    u8 b = cmos_read(REG_STATUS_B);
    if (a == 0xFF && b == 0xFF) return false;

    binary_mode = (b & STATUS_B_BINARY) != 0;
    hour24      = (b & STATUS_B_24HOUR) != 0;

    rtc_time_t t;
    if (!rtc_read(&t)) return false;

    /* Sanity, because a plausible looking clock that is wrong is worse than
       one that says it is not there. */
    if (t.month < 1 || t.month > 12) return false;
    if (t.day < 1 || t.day > 31) return false;
    if (t.hour > 23 || t.minute > 59 || t.second > 59) return false;

    present = true;
    return true;
}

bool rtc_read(rtc_time_t *out) {
    if (!out) return false;

    /* Bounded, so a chip that reports an update forever does not hang the
       boot. A real update takes under two milliseconds. */
    u32 spins = 0;
    while (updating() && spins++ < 1000000) { }
    if (spins >= 1000000) return false;

    rtc_time_t first, second;
    sample(&first);

    /* Read it again and require agreement. A mismatch means the second
       changed underneath the first read, so try once more. */
    for (int tries = 0; tries < 8; tries++) {
        spins = 0;
        while (updating() && spins++ < 1000000) { }
        sample(&second);
        if (same(&first, &second)) break;
        first = second;
    }

    rtc_time_t t = second;

    if (!binary_mode) {
        t.second  = from_bcd(t.second);
        t.minute  = from_bcd(t.minute);
        t.day     = from_bcd(t.day);
        t.month   = from_bcd(t.month);
        t.year    = (u32)from_bcd((u8)t.year);
        t.weekday = from_bcd(t.weekday);
        /* The hour's top bit is the afternoon flag in twelve hour mode and
           has to survive the conversion, so it is masked off first and put
           back afterwards. */
        u8 pm = (u8)(t.hour & 0x80);
        t.hour = (u8)(from_bcd((u8)(t.hour & 0x7F)) | pm);
    }

    if (!hour24) {
        u8 pm = (u8)(t.hour & 0x80);
        u8 h = (u8)(t.hour & 0x7F);
        if (h == 12) h = 0;                 /* twelve is the zero hour */
        t.hour = pm ? (u8)(h + 12) : h;
    }

    /* Two digits of year, and no century register this can rely on: the one
       at 0x32 is only meaningful when ACPI's FADT says so, and plenty of
       firmware leaves it as something else entirely. Below 70 is this
       century, which is the same assumption everything else made in 1999 and
       is good until 2070. */
    if (t.year < 70) t.year += 2000;
    else             t.year += 1900;

    *out = t;
    return true;
}

static void two(char *out, int at, u32 v) {
    out[at]     = (char)('0' + (v / 10) % 10);
    out[at + 1] = (char)('0' + v % 10);
}

void rtc_format(char *out, u32 cap) {
    if (cap < 20) { if (cap) out[0] = 0; return; }

    rtc_time_t t;
    if (!rtc_read(&t)) { memcpy(out, "no clock", 9); return; }

    out[0] = (char)('0' + (t.year / 1000) % 10);
    out[1] = (char)('0' + (t.year / 100) % 10);
    out[2] = (char)('0' + (t.year / 10) % 10);
    out[3] = (char)('0' + t.year % 10);
    out[4] = '-'; two(out, 5, t.month);
    out[7] = '-'; two(out, 8, t.day);
    out[10] = ' ';
    two(out, 11, t.hour);
    out[13] = ':'; two(out, 14, t.minute);
    out[16] = ':'; two(out, 17, t.second);
    out[19] = 0;
}

void rtc_format_short(char *out, u32 cap) {
    if (cap < 6) { if (cap) out[0] = 0; return; }

    rtc_time_t t;
    if (!rtc_read(&t)) { memcpy(out, "--:--", 6); return; }

    two(out, 0, t.hour);
    out[2] = ':';
    two(out, 3, t.minute);
    out[5] = 0;
}
