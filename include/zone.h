#pragma once
#include "types.h"
#include "rtc.h"

/* Where the machine is, as far as its clock goes.
 *
 * The CMOS clock is read as UTC, which is what QEMU keeps in it and what the
 * browser's Date has always taken it to be. The desktop's clock showed that
 * as it was, so everybody not in London in winter read the wrong hour off
 * the dock. A zone is a line in /zelr.cfg ("zone Europe/Paris", read by
 * theme.c and chosen on Settings' Time page); the names and the summer rules
 * are userland/zones.h, the same table the browser's Intl keeps, so the dock
 * and a page cannot disagree. A machine nobody has told is in UTC.
 *
 * A clock that keeps local time instead, as Windows sets the clock of a
 * machine it shares and some virtual machines set theirs, is the theme's
 * clock_local ("Clock keeps local time" on the Time page): its reading is
 * then the wall clock in the zone, and UTC is worked out from it. */

typedef struct {
    rtc_time_t local;    /* the wall clock in the zone */
    rtc_time_t at_utc;   /* the same moment in UTC */
    i64  utc;            /* seconds since 1970, UTC */
    int  offset;         /* minutes east of UTC at that moment */
    bool summer;         /* whether that is the zone's summer time */
} zone_time_t;

/* By the name IANA gives it, in any case. False, and the zone left as it
   was, for a name this does not know. */
bool zone_set(const char *name);

/* The zone's name as the table writes it ("UTC" until one is set). */
const char *zone_name(void);

/* The names known, for the shell to list: 0 past the last. */
const char *zone_name_at(int i);

/* Seconds since 1970 of a time read off the clock, taken as UTC. */
i64 zone_utc_of(const rtc_time_t *t);

/* A moment as it is in the zone set. */
void zone_at(i64 utc, zone_time_t *out);

/* Now, in the zone set; false when there is no clock to read. */
bool zone_now(zone_time_t *out);

/* A reading off the clock as zone_now takes it: UTC, or with clock_local the
   wall clock in the zone. Where the clocks go forward an hour a local
   reading in the gap is read on the offset from before; where they go back,
   as the first of the two moments. */
void zone_from_reading(const rtc_time_t *reading, zone_time_t *out);

/* An offset as people write it: "UTC", "UTC+10", "UTC-3:30". */
void zone_offset_text(int offset, char *out, u32 cap);

/* Seconds added to every reading of the clock: what timesync.c found the
   clock out by, from a time server. Nought until one has answered. */
void zone_set_adjust(i64 seconds);
i64  zone_adjust(void);

/* For the self test: zone_now answers this moment (seconds since 1970)
   rather than the clock's, until it is given -1. Files written meanwhile
   are stamped with it. */
void zone_test_fix(i64 utc);
