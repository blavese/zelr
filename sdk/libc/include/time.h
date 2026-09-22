/* time.h -- Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Enough to measure with. There is no calendar here: the machine knows how
 * long it has been up and what the clock says, and turning that into a
 * broken-down local time needs a time zone database, which is a thing to
 * ship rather than a thing to invent. */
#pragma once
#include <stddef.h>

typedef long long time_t;
typedef long long clock_t;

#define CLOCKS_PER_SEC 1000

time_t  time(time_t *out);
clock_t clock(void);
