#pragma once
#include "types.h"

/* ECDSA over NIST P-256, which is what the certificate at the end of most
 * chains is keyed with, and what the server signs the handshake with.
 *
 * Verification only, as with RSA: the numbers are all public, so the
 * arithmetic is allowed to be the straightforward kind.
 *
 * The public key arrives as the uncompressed point encoding: a leading 0x04
 * and then the two coordinates, thirty two bytes each. */

#define P256_POINT_LEN 65
#define P256_SCALAR_LEN 32

/* Whether (r, s) is a signature on hash by the holder of this key. False
 * covers every kind of no there is: a malformed point, a point that is not
 * on the curve, a scalar out of range, and a signature that simply does not
 * check out. Telling those apart would be useful for a diagnostic and is
 * exactly what a caller must not be able to act on differently. */
bool p256_verify(const u8 pubkey[P256_POINT_LEN],
                 const u8 hash[32],
                 const u8 *r, u32 r_len,
                 const u8 *s, u32 s_len);

/* k times the base point, as the x coordinate of the result, or false when
   the result is the point at infinity. Not needed to verify a signature: it
   is here because the curve arithmetic underneath is the part most likely
   to be subtly wrong, and this is the part of it with published answers. */
bool p256_base_x(const u8 k[P256_SCALAR_LEN], u8 out_x[32]);
