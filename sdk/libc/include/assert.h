/* assert.h -- Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Deliberately not #pragma once: the standard says including it again with
 * NDEBUG set differently changes what assert means, and a program that
 * does that is entitled to. */
#undef assert

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
void _zassert(const char *what, const char *file, int line);
#define assert(e) ((e) ? (void)0 : _zassert(#e, __FILE__, __LINE__))
#endif
