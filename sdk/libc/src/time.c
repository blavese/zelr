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
   1000 promises. */
clock_t clock(void) {
    return (clock_t)ticks();
}

/* The standard's answer for "this machine cannot tell you" is (time_t)-1,
   and it is a real answer rather than a failure to have one. */
time_t time(time_t *out) {
    if (out) *out = (time_t)-1;
    return (time_t)-1;
}
