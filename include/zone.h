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
 * A clock that keeps local time (as Windows sets it on a machine it shares)
 * is not allowed for: the zone is added to whatever the clock says. */

typedef struct {
    rtc_time_t local;    /* the wall clock in the zone */
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

/* An offset as people write it: "UTC", "UTC+10", "UTC-3:30". */
void zone_offset_text(int offset, char *out, u32 cap);

/* For the self test: zone_now answers this moment (seconds since 1970)
   rather than the clock's, until it is given -1. Files written meanwhile
   are stamped with it. */
void zone_test_fix(i64 utc);
