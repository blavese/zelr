#pragma once
/* Time zones: which clock a place keeps, by name, and when its summer is.
 *
 * Shared by the kernel (kernel/zone.c: the desktop's clock, /sys/time) and
 * the browser's Intl (jsintl.h), so the clock on the dock and a page's
 * dates cannot disagree about when summer starts; Settings lists these
 * names. Whole numbers only, because the kernel has no floating point.
 *
 * Seventy one zones and four rules for summer, which is what most of the
 * world's clocks do now. A zone's history is not here (America's rules
 * before 2007, say), so a moment years back is told by today's rules. */

enum { TZR_NONE, TZR_US, TZR_EU, TZR_AU, TZR_NZ };

/* Each includer uses some of these and not the rest. */
#define ZONE_FN static __attribute__((unused))

typedef struct {
    const char *name;
    int offset;                 /* minutes east of UTC, outside summer */
    int rule;
    const char *s_std, *s_dst;  /* short names, or 0 for GMT+N */
    const char *l_std, *l_dst;  /* long names, or 0 for GMT+NN:NN */
} zone_t;

static const zone_t ZONES[] = {
    { "UTC", 0, TZR_NONE, "UTC", "UTC", "Coordinated Universal Time", "Coordinated Universal Time" },
    { "America/New_York", -300, TZR_US, "EST", "EDT", "Eastern Standard Time", "Eastern Daylight Time" },
    { "America/Detroit", -300, TZR_US, "EST", "EDT", "Eastern Standard Time", "Eastern Daylight Time" },
    { "America/Toronto", -300, TZR_US, "EST", "EDT", "Eastern Standard Time", "Eastern Daylight Time" },
    { "America/Chicago", -360, TZR_US, "CST", "CDT", "Central Standard Time", "Central Daylight Time" },
    { "America/Winnipeg", -360, TZR_US, "CST", "CDT", "Central Standard Time", "Central Daylight Time" },
    { "America/Denver", -420, TZR_US, "MST", "MDT", "Mountain Standard Time", "Mountain Daylight Time" },
    { "America/Edmonton", -420, TZR_US, "MST", "MDT", "Mountain Standard Time", "Mountain Daylight Time" },
    { "America/Phoenix", -420, TZR_NONE, "MST", "MST", "Mountain Standard Time", "Mountain Standard Time" },
    { "America/Los_Angeles", -480, TZR_US, "PST", "PDT", "Pacific Standard Time", "Pacific Daylight Time" },
    { "America/Vancouver", -480, TZR_US, "PST", "PDT", "Pacific Standard Time", "Pacific Daylight Time" },
    { "America/Anchorage", -540, TZR_US, "AKST", "AKDT", "Alaska Standard Time", "Alaska Daylight Time" },
    { "Pacific/Honolulu", -600, TZR_NONE, "HST", "HST", "Hawaii-Aleutian Standard Time", "Hawaii-Aleutian Standard Time" },
    { "America/Halifax", -240, TZR_US, "AST", "ADT", "Atlantic Standard Time", "Atlantic Daylight Time" },
    { "America/Mexico_City", -360, TZR_NONE, "CST", "CST", "Central Standard Time", "Central Standard Time" },
    { "America/Bogota", -300, TZR_NONE, 0, 0, "Colombia Standard Time", "Colombia Standard Time" },
    { "America/Lima", -300, TZR_NONE, 0, 0, "Peru Standard Time", "Peru Standard Time" },
    { "America/Sao_Paulo", -180, TZR_NONE, 0, 0, "Brasilia Standard Time", "Brasilia Standard Time" },
    { "America/Argentina/Buenos_Aires", -180, TZR_NONE, 0, 0, "Argentina Standard Time", "Argentina Standard Time" },
    { "Europe/London", 0, TZR_EU, "GMT", 0, "Greenwich Mean Time", "British Summer Time" },
    { "Europe/Dublin", 0, TZR_EU, "GMT", 0, "Greenwich Mean Time", "Irish Standard Time" },
    { "Europe/Lisbon", 0, TZR_EU, 0, 0, "Western European Standard Time", "Western European Summer Time" },
    { "Europe/Paris", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Berlin", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Madrid", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Rome", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Amsterdam", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Brussels", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Vienna", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Zurich", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Stockholm", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Oslo", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Copenhagen", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Warsaw", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Prague", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Budapest", 60, TZR_EU, 0, 0, "Central European Standard Time", "Central European Summer Time" },
    { "Europe/Athens", 120, TZR_EU, 0, 0, "Eastern European Standard Time", "Eastern European Summer Time" },
    { "Europe/Helsinki", 120, TZR_EU, 0, 0, "Eastern European Standard Time", "Eastern European Summer Time" },
    { "Europe/Kyiv", 120, TZR_EU, 0, 0, "Eastern European Standard Time", "Eastern European Summer Time" },
    { "Europe/Kiev", 120, TZR_EU, 0, 0, "Eastern European Standard Time", "Eastern European Summer Time" },
    { "Europe/Bucharest", 120, TZR_EU, 0, 0, "Eastern European Standard Time", "Eastern European Summer Time" },
    { "Europe/Istanbul", 180, TZR_NONE, 0, 0, 0, 0 },
    { "Europe/Moscow", 180, TZR_NONE, 0, 0, "Moscow Standard Time", "Moscow Standard Time" },
    { "Africa/Lagos", 60, TZR_NONE, 0, 0, "West Africa Standard Time", "West Africa Standard Time" },
    { "Africa/Johannesburg", 120, TZR_NONE, 0, 0, "South Africa Standard Time", "South Africa Standard Time" },
    { "Africa/Nairobi", 180, TZR_NONE, 0, 0, "East Africa Time", "East Africa Time" },
    { "Asia/Dubai", 240, TZR_NONE, 0, 0, "Gulf Standard Time", "Gulf Standard Time" },
    { "Asia/Karachi", 300, TZR_NONE, 0, 0, "Pakistan Standard Time", "Pakistan Standard Time" },
    { "Asia/Kolkata", 330, TZR_NONE, 0, 0, "India Standard Time", "India Standard Time" },
    { "Asia/Calcutta", 330, TZR_NONE, 0, 0, "India Standard Time", "India Standard Time" },
    { "Asia/Kathmandu", 345, TZR_NONE, 0, 0, "Nepal Time", "Nepal Time" },
    { "Asia/Dhaka", 360, TZR_NONE, 0, 0, "Bangladesh Standard Time", "Bangladesh Standard Time" },
    { "Asia/Bangkok", 420, TZR_NONE, 0, 0, "Indochina Time", "Indochina Time" },
    { "Asia/Jakarta", 420, TZR_NONE, 0, 0, "Western Indonesia Time", "Western Indonesia Time" },
    { "Asia/Ho_Chi_Minh", 420, TZR_NONE, 0, 0, "Indochina Time", "Indochina Time" },
    { "Asia/Shanghai", 480, TZR_NONE, 0, 0, "China Standard Time", "China Standard Time" },
    { "Asia/Hong_Kong", 480, TZR_NONE, 0, 0, "Hong Kong Standard Time", "Hong Kong Standard Time" },
    { "Asia/Singapore", 480, TZR_NONE, 0, 0, "Singapore Standard Time", "Singapore Standard Time" },
    { "Asia/Taipei", 480, TZR_NONE, 0, 0, "Taipei Standard Time", "Taipei Standard Time" },
    { "Asia/Manila", 480, TZR_NONE, 0, 0, "Philippine Standard Time", "Philippine Standard Time" },
    { "Australia/Perth", 480, TZR_NONE, 0, 0, "Australian Western Standard Time", "Australian Western Standard Time" },
    { "Asia/Seoul", 540, TZR_NONE, 0, 0, "Korean Standard Time", "Korean Standard Time" },
    { "Asia/Tokyo", 540, TZR_NONE, 0, 0, "Japan Standard Time", "Japan Standard Time" },
    { "Australia/Darwin", 570, TZR_NONE, 0, 0, "Australian Central Standard Time", "Australian Central Standard Time" },
    { "Australia/Adelaide", 570, TZR_AU, 0, 0, "Australian Central Standard Time", "Australian Central Daylight Time" },
    { "Australia/Brisbane", 600, TZR_NONE, 0, 0, "Australian Eastern Standard Time", "Australian Eastern Standard Time" },
    { "Australia/Sydney", 600, TZR_AU, 0, 0, "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
    { "Australia/Melbourne", 600, TZR_AU, 0, 0, "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
    { "Australia/Canberra", 600, TZR_AU, 0, 0, "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
    { "Australia/Hobart", 600, TZR_AU, 0, 0, "Australian Eastern Standard Time", "Australian Eastern Daylight Time" },
    { "Pacific/Auckland", 720, TZR_NZ, 0, 0, "New Zealand Standard Time", "New Zealand Daylight Time" },
    { 0, 0, 0, 0, 0, 0, 0 },
};

/* Days since 1970-01-01 of a day of the Gregorian calendar, month 1..12:
   Howard Hinnant's arithmetic, for any year. */
ZONE_FN long long zone_days_from_civil(long long y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long mp = m > 2 ? m - 3 : m + 9;
    long long doy = (153 * mp + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

ZONE_FN void zone_civil_from_days(long long z, long long *y, int *m, int *d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

/* The day a moment (seconds since 1970, UTC) falls on, rounding down: C's
   division rounds towards nought, which before 1970 is the day after. */
ZONE_FN long long zone_day_of(long long t) {
    long long d = t / 86400;
    return t % 86400 < 0 ? d - 1 : d;
}

/* 0 is Sunday. 1970-01-01 was a Thursday. */
ZONE_FN int zone_weekday(long long day) {
    long long w = (day + 4) % 7;
    return (int)(w < 0 ? w + 7 : w);
}

/* The moment of the nth Sunday of a month (1..12; nth from 1, or 0 for the
   last), at `hour` on a clock `off` minutes east of UTC. */
ZONE_FN long long zone_sunday(long long year, int month, int nth, int hour, int off) {
    long long first = zone_days_from_civil(year, month, 1);
    long long day = (7 - zone_weekday(first)) % 7;
    if (nth > 0) day += 7 * (nth - 1);
    else {
        long long len = zone_days_from_civil(month == 12 ? year + 1 : year, month == 12 ? 1 : month + 1, 1) - first;
        while (day + 7 < len) day += 7;
    }
    return (first + day) * 86400 + hour * 3600LL - off * 60LL;
}

/* Whether a zone keeps summer time at a moment. Each rule starts and ends
   at the hour that zone's law says, on its own clock (or, in Europe, at one
   in the morning in UTC everywhere at once). */
ZONE_FN int zone_in_summer(const zone_t *z, long long t) {
    if (z->rule == TZR_NONE) return 0;
    long long y;
    int m, d;
    zone_civil_from_days(zone_day_of(t), &y, &m, &d);
    int o = z->offset;
    switch (z->rule) {
        case TZR_US: return t >= zone_sunday(y, 3, 2, 2, o) && t < zone_sunday(y, 11, 1, 1, o);
        case TZR_EU: return t >= zone_sunday(y, 3, 0, 1, 0) && t < zone_sunday(y, 10, 0, 1, 0);
        case TZR_AU: return t < zone_sunday(y, 4, 1, 2, o) || t >= zone_sunday(y, 10, 1, 2, o);
        case TZR_NZ: return t < zone_sunday(y, 4, 1, 2, o) || t >= zone_sunday(y, 9, 0, 2, o);
    }
    return 0;
}

/* Minutes east of UTC that a zone's clocks are at a moment. */
ZONE_FN int zone_offset_at(const zone_t *z, long long t) {
    return z->offset + (zone_in_summer(z, t) ? 60 : 0);
}

ZONE_FN char zone_low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

ZONE_FN int zone_same(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i] && zone_low(a[i]) == zone_low(b[i])) i++;
    return !a[i] && !b[i];
}

/* A zone by name, as IANA writes it or in any case; -1 when it is none
   this knows. UTC's other names are UTC. */
ZONE_FN int zone_find(const char *name) {
    static const char *const UTCS[] = { "utc", "etc/utc", "gmt", "etc/gmt", "etc/universal", "universal", "zulu", "etc/zulu", 0 };
    for (int i = 0; UTCS[i]; i++) if (zone_same(name, UTCS[i])) return 0;
    for (int i = 0; ZONES[i].name; i++) if (zone_same(name, ZONES[i].name)) return i;
    return -1;
}

ZONE_FN int zone_count(void) {
    int n = 0;
    while (ZONES[n].name) n++;
    return n;
}
