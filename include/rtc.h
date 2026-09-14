#pragma once
#include "types.h"

/* The clock the machine keeps while it is switched off.
 *
 * Everything else here counts from boot: the timer says how many hundredths
 * of a second have passed since the scheduler started, which is enough to
 * schedule with and useless for saying when a file was written. The CMOS
 * clock is the only thing on a PC that knows the actual time, and it has
 * been in the same place since 1984.
 *
 * It is read rather than set. Setting it means deciding what timezone the
 * machine is in, which means a timezone database, and there is nothing here
 * yet that would use the answer. */

typedef struct {
    u32 year;        /* full, not two digits */
    u8  month;       /* 1..12 */
    u8  day;         /* 1..31 */
    u8  hour;        /* 0..23 */
    u8  minute;
    u8  second;
    u8  weekday;     /* 0 is Sunday; 0 when the chip does not say */
} rtc_time_t;

bool rtc_init(void);
bool rtc_present(void);

/* The time now. False if there is no usable clock, in which case the fields
   are left alone. */
bool rtc_read(rtc_time_t *out);

/* "2026-09-14 22:41:07", into at least 20 bytes. */
void rtc_format(char *out, u32 cap);

/* Just the wall clock, "22:41", which is what a panel wants. */
void rtc_format_short(char *out, u32 cap);
