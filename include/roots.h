#pragma once
#include "types.h"
#include "string.h"

/* The trust anchors: the authorities whose signature this machine accepts
 * as the end of a chain.
 *
 * This is the one part of https that cannot be derived. Every other piece
 * here is arithmetic with a right answer, and a mistake in it shows up as a
 * failure. This is a list of who to believe, and the only thing that makes
 * it correct is that it agrees with the list everybody else is using. So it
 * is generated from a published store rather than chosen here, and what it
 * contains is public certificates: a name, a public key, and a signature
 * over the pair. Nothing in them is executed.
 *
 * A machine that trusts nobody rejects the whole web. A machine that trusts
 * everybody has encryption and no idea who it is talking to, which is the
 * more dangerous of the two because it looks like it works. */

typedef struct {
    const u8 *der;
    u32 len;
    u16 sub_off;      /* where the subject name starts inside der */
    u16 sub_len;
} root_t;

u32 roots_count(void);
const root_t *roots_at(u32 i);

/* The certificate of the authority with this subject name, if it is one
   this machine trusts. The name is compared as its raw encoding. */
bool roots_find(const u8 *subject, u32 len, const u8 **out, u32 *out_len);
