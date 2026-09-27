/* Measuring how long something took.
 *
 * Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * There is no calendar here and time() says so. The kernel counts ticks
 * since it started and has no idea what year it is; turning a count of
 * ticks into a date would mean inventing an epoch, and a program told it
 * is 1970 behaves worse than a program told nobody knows.
 *
 * clock() is the one that works, and it is the one a program measuring
 * itself actually calls.
 */
#include <time.h>

/* The build script sets both of these; they are here so that this
   file is also correct when compiled on its own. */
#ifndef ZELR_NO_SUGAR
#define ZELR_NO_SUGAR 1
#endif
#ifndef ZELR_NO_START
#define ZELR_NO_START 1
#endif
#include "zelr.h"

/* Milliseconds since this machine started, which is what CLOCKS_PER_SEC of
   1000 promises.
 *
 * ticks() counts the timer, which runs at a hundred a second, and this
 * returned it as it was: every duration a program measured came out a tenth
 * of its length. The rate is read from /sys/uptime rather than written down
 * here, so a kernel that changes it does not quietly break this again. */
static long tick_hz;

static long hz_now(void) {
    if (tick_hz) return tick_hz;
    char buf[256];
    int n = slurp("/sys/uptime", buf, (int)sizeof(buf) - 1);
    long hz = 0;
    if (n > 0) {
        buf[n] = 0;
        /* "... at N Hz" */
        for (int i = 0; i + 3 < n; i++) {
            if (buf[i] != 'a' || buf[i + 1] != 't' || buf[i + 2] != ' ') continue;
            long v = 0;
            int j = i + 3;
            while (j < n && buf[j] >= '0' && buf[j] <= '9') v = v * 10 + (buf[j++] - '0');
            if (v > 0 && j + 3 <= n && buf[j] == ' ' && buf[j + 1] == 'H' && buf[j + 2] == 'z') {
                hz = v;
                break;
            }
        }
    }
    tick_hz = hz > 0 ? hz : 100;
    return tick_hz;
}

clock_t clock(void) {
    return (clock_t)((long long)ticks() * 1000 / hz_now());
}

/* The standard's answer for "this machine cannot tell you" is (time_t)-1,
   and it is a real answer rather than a failure to have one. */
time_t time(time_t *out) {
    if (out) *out = (time_t)-1;
    return (time_t)-1;
}
