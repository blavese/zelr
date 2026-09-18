#pragma once
#include "types.h"

/* X25519, the key agreement TLS 1.3 uses by default.
 *
 * Two machines that have never met each other end up holding the same
 * thirty two bytes, having sent each other only a public value, and anybody
 * who watched the whole exchange cannot work out what they agreed on. That
 * is the entire basis of the connection being private, and it is one
 * function.
 *
 * The arithmetic is modulo 2^255 - 19, which does not fit in any integer
 * the machine has, so a number is held as five limbs of fifty one bits and
 * multiplication is done in 128 bit intermediates. The reduction is nearly
 * free, which is why that prime was chosen. */

#define X25519_LEN 32

/* Turns a private scalar into the public value to send. */
void x25519_public(const u8 secret[X25519_LEN], u8 out[X25519_LEN]);

/* And a private scalar plus the value the other side sent into the shared
   secret. False when the peer's value is one of the small order points,
   which produce an all zero secret: that is a peer forcing a known key
   rather than an unlucky one, and it has to be refused. */
bool x25519(const u8 secret[X25519_LEN], const u8 peer[X25519_LEN],
            u8 out[X25519_LEN]);
