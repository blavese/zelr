#pragma once
#include "types.h"

/* RSA signature verification, which is how most of the certificate chain
 * behind an https address is held together.
 *
 * Only verification. There is no private key here and nothing signs
 * anything, so none of this has to resist someone measuring how long it
 * took: every number involved is public, and the arithmetic is allowed to
 * be the fast obvious kind.
 *
 * The modulus is up to 4096 bits because the roots go that big. */

#define RSA_MAX_BYTES 512

typedef struct {
    u8  n[RSA_MAX_BYTES];   /* modulus, big end first, no leading zeros */
    u32 n_len;
    u32 e;                  /* the public exponent, which is small in practice */
} rsa_key_t;

/* The two paddings that turn up. PKCS#1 v1.5 is what nearly every
   certificate is signed with; PSS is what TLS 1.3 requires for the
   signature the server makes over the handshake itself, so both are needed
   to reach an ordinary web site. */
bool rsa_verify_pkcs1(const rsa_key_t *key, const u8 *sig, u32 sig_len,
                      const u8 *hash, u32 hash_len);
bool rsa_verify_pss(const rsa_key_t *key, const u8 *sig, u32 sig_len,
                    const u8 *hash, u32 hash_len);
