#pragma once
#include "types.h"

/* SHA-384, and the SHA-512 it is a truncation of.
 *
 * Needed because several of the large certificate authorities sign their
 * intermediates with it. A client that has only SHA-256 reaches google.com
 * and fails on cloudflare, which is not a useful place to stop.
 *
 * The same shape as SHA-256 with everything twice as wide: sixty four bit
 * words, eighty rounds, a different set of constants and a different
 * starting state. SHA-384 is SHA-512 with another starting state and the
 * answer cut in half. */

#define SHA384_SIZE 48
#define SHA512_SIZE 64
#define SHA512_BLOCK 128

typedef struct {
    u64 h[8];
    u64 bits;                 /* this will not overflow on anything real */
    u8  buf[SHA512_BLOCK];
    u32 have;
    u32 out_len;
} sha512_t;

void sha384_init(sha512_t *s);
void sha512_init(sha512_t *s);
void sha512_update(sha512_t *s, const void *data, u32 len);
void sha512_final(sha512_t *s, u8 *out);      /* out_len bytes */

void sha384(const void *data, u32 len, u8 out[SHA384_SIZE]);
void sha512(const void *data, u32 len, u8 out[SHA512_SIZE]);
