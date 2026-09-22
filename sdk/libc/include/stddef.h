/* stddef.h -- the types every other header here needs.
 *
 * Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * These come from the compiler rather than from a number written here: the
 * width of a size_t is a property of the target, and a header that guesses
 * it is a header that is wrong on the next one. */
#pragma once

typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __WCHAR_TYPE__   wchar_t;

#define NULL ((void *)0)

#define offsetof(t, m) __builtin_offsetof(t, m)

typedef long double max_align_t;
