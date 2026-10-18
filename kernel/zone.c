/* See include/zone.h. */
#include "zone.h"
#include "printf.h"
#include "theme.h"
#include "../userland/zones.h"

/* Into ZONES; 0 is UTC. */
static int current;
static i64 fixed = -1;               /* see zone_test_fix */

bool zone_set(const char *name) {
    int i = zone_find(name);
    if (i < 0) return false;
    current = i;
    return true;
}

const char *zone_name(void) { return ZONES[current].name; }

const char *zone_name_at(int i) {
    return i >= 0 && i < zone_count() ? ZONES[i].name : 0;
}

i64 zone_utc_of(const rtc_time_t *t) {
    return zone_days_from_civil(t->year, t->month, t->day) * 86400
           + t->hour * 3600 + t->minute * 60 + t->second;
}

static void civil(i64 t, rtc_time_t *out) {
    i64 day = zone_day_of(t);
    i64 rest = t - day * 86400;
    long long y;
    int m, d;
    zone_civil_from_days(day, &y, &m, &d);
    out->year = (u32)y;
    out->month = (u8)m;
    out->day = (u8)d;
    out->hour = (u8)(rest / 3600);
    out->minute = (u8)(rest / 60 % 60);
    out->second = (u8)(rest % 60);
    out->weekday = (u8)zone_weekday(day);
}

void zone_at(i64 utc, zone_time_t *out) {
    const zone_t *z = &ZONES[current];
    int off = zone_offset_at(z, utc);
    civil(utc + off * 60, &out->local);
    civil(utc, &out->at_utc);
    out->utc = utc;
    out->offset = off;
    out->summer = off != z->offset;
}

void zone_test_fix(i64 utc) { fixed = utc; }

void zone_from_reading(const rtc_time_t *reading, zone_time_t *out) {
    i64 r = zone_utc_of(reading);
    if (theme()->clock_local) {
        const zone_t *z = &ZONES[current];
        i64 summer = r - (z->offset + 60) * 60;
        if (z->rule != TZR_NONE && zone_offset_at(z, summer) == z->offset + 60) r = summer;
        else r -= z->offset * 60;
    }
    zone_at(r, out);
}

bool zone_now(zone_time_t *out) {
    if (fixed >= 0) { zone_at(fixed, out); return true; }
    rtc_time_t t;
    if (!rtc_present() || !rtc_read(&t)) return false;
    zone_from_reading(&t, out);
    return true;
}

void zone_offset_text(int offset, char *out, u32 cap) {
    int a = offset < 0 ? -offset : offset;
    if (!offset) kformat(out, cap, "UTC");
    else if (a % 60) kformat(out, cap, "UTC%c%d:%02d", offset < 0 ? '-' : '+', a / 60, a % 60);
    else kformat(out, cap, "UTC%c%d", offset < 0 ? '-' : '+', a / 60);
}
