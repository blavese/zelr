#pragma once
#include "types.h"

/* ECDSA over the two NIST curves the web actually uses, and key agreement.
 *
 * P-256 is what most leaf certificates are keyed with and what a server
 * usually signs a TLS handshake with. P-384 turns up one level up: several
 * of the large authorities key their intermediates with it, so a client
 * that has only the first curve can reach google.com and not cloudflare.
 *
 * Checking a signature, every number involved is public, so the scalar loop
 * is allowed to be the straightforward kind. Agreeing a key is not: the
 * private scalar goes through a loop that does the same work for every bit
 * (pt_mul_secret), on field arithmetic that does not branch on its carries.
 * Some servers agree keys on P-256 and not on x25519: Microsoft's sign-in
 * and mail, Craigslist, Ars Technica.
 *
 * A key arrives as the uncompressed point encoding: a leading 0x04 and then
 * the two coordinates, each as wide as the curve. */

typedef enum { EC_P256, EC_P384 } ec_curve_t;

/* How wide a coordinate is, so a caller can check a key's length before
   handing it over. */
u32 ec_field_bytes(ec_curve_t curve);

/* Whether (r, s) is a signature on hash by the holder of this key.
 *
 * False covers every kind of no there is: a key that is not a point, a
 * point that is not on the curve, a scalar out of range, and a signature
 * that simply does not check out. Telling those apart would make a nicer
 * diagnostic and is exactly what a caller must not be able to act on. */
bool ec_verify(ec_curve_t curve,
               const u8 *pubkey, u32 pubkey_len,
               const u8 *hash, u32 hash_len,
               const u8 *r, u32 r_len,
               const u8 *s, u32 s_len);

/* k times the base point, as the x coordinate, or false for the point at
   infinity. Not needed to check a signature: it is here because the curve
   arithmetic underneath is the part most likely to be quietly wrong, and
   this is the part of it with published answers to compare against. */
bool ec_base_x(ec_curve_t curve, const u8 *k, u32 k_len, u8 *out_x);

/* The public point for a private scalar (as wide as the curve): pub gets the
   uncompressed encoding, 1 + 2 * width bytes. False when the bytes are not a
   scalar between one and the order, and the caller draws others. */
bool ec_public(ec_curve_t curve, const u8 *priv, u32 priv_len, u8 *pub);

/* The secret agreed with the holder of peer (an uncompressed point): the x
   coordinate of priv times it, as wide as the curve. False for a point that
   is not on the curve, in any encoding but the uncompressed one, or a scalar
   out of range. */
bool ec_agree(ec_curve_t curve, const u8 *priv, u32 priv_len,
              const u8 *peer, u32 peer_len, u8 *out_x);
