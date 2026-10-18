/* See include/zone.h. */
#include "zone.h"
#include "printf.h"
#include "../userland/zones.h"

/* Into ZONES; 0 is UTC. */
static int current;

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

void zone_at(i64 utc, zone_time_t *out) {
    const zone_t *z = &ZONES[current];
    int off = zone_offset_at(z, utc);
    i64 local = utc + off * 60;
    i64 day = zone_day_of(local);
    i64 rest = local - day * 86400;
    long long y;
    int m, d;
    zone_civil_from_days(day, &y, &m, &d);
    out->local.year = (u32)y;
    out->local.month = (u8)m;
    out->local.day = (u8)d;
    out->local.hour = (u8)(rest / 3600);
    out->local.minute = (u8)(rest / 60 % 60);
    out->local.second = (u8)(rest % 60);
    out->local.weekday = (u8)zone_weekday(day);
    out->utc = utc;
    out->offset = off;
    out->summer = off != z->offset;
}

bool zone_now(zone_time_t *out) {
    rtc_time_t t;
    if (!rtc_present() || !rtc_read(&t)) return false;
    zone_at(zone_utc_of(&t), out);
    return true;
}

void zone_offset_text(int offset, char *out, u32 cap) {
    int a = offset < 0 ? -offset : offset;
    if (!offset) kformat(out, cap, "UTC");
    else if (a % 60) kformat(out, cap, "UTC%c%d:%02d", offset < 0 ? '-' : '+', a / 60, a % 60);
    else kformat(out, cap, "UTC%c%d", offset < 0 ? '-' : '+', a / 60);
}
