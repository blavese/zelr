#pragma once
#include "types.h"

/* One buffer, shared by everything.
 *
 * Copy and paste is the least glamorous thing a desktop needs and the first
 * one anybody notices is missing. It belongs in the kernel rather than in a
 * program for the same reason the window server does: two ring 3 programs
 * have no way to hand each other bytes, and the whole point is that the
 * terminal can copy something the text editor pastes.
 *
 * It is deliberately only text. A clipboard that carries arbitrary types
 * needs a type negotiation, which needs a protocol, which is a great deal of
 * machinery for a system whose programs all currently deal in characters. */

#define CLIP_MAX 65536

void clip_init(void);

/* Replaces what is there. Longer than CLIP_MAX is refused rather than
   truncated: a paste that silently loses the end of a file is worse than one
   that does not happen. */
bool clip_set(const char *text, u32 len);

/* Copies out at most cap bytes and returns how many. Always terminates when
   there is room. */
u32  clip_get(char *out, u32 cap);

u32  clip_len(void);
u32  clip_generation(void);   /* bumped on every set, so a watcher can tell */
