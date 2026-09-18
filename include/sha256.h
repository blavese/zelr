#pragma once
#include "types.h"

/* SHA-256, and the two things TLS builds out of it.
 *
 * The existing crypto.c has SHA-1, because that is what WPA2 is specified in
 * terms of. TLS 1.3 is specified in terms of this one and will not accept
 * the other, so both are here rather than one pretending to be the other.
 *
 * None of this is invented and none of it should be. Every function below
 * has published answers to published inputs, and kernel/selftest.c checks
 * against those rather than against itself. */

#define SHA256_SIZE 32
#define SHA256_BLOCK 64

typedef struct {
    u32 h[8];
    u64 bits;
    u8  buf[SHA256_BLOCK];
    u32 have;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, u32 len);
void sha256_final(sha256_t *s, u8 out[SHA256_SIZE]);
void sha256(const void *data, u32 len, u8 out[SHA256_SIZE]);

void hmac_sha256(const u8 *key, u32 key_len,
                 const u8 *data, u32 data_len, u8 out[SHA256_SIZE]);

/* HKDF, which is how TLS 1.3 turns one shared secret into every key it
   needs. Extract concentrates whatever entropy the input has into one
   block; expand stretches that into as many distinct keys as are asked for,
   each labelled so that two keys for different purposes can never come out
   the same. */
void hkdf_extract(const u8 *salt, u32 salt_len,
                  const u8 *ikm, u32 ikm_len, u8 out[SHA256_SIZE]);
bool hkdf_expand(const u8 prk[SHA256_SIZE], const u8 *info, u32 info_len,
                 u8 *out, u32 out_len);

/* The labelled form TLS 1.3 uses, which is hkdf_expand over a structure
   holding the length, "tls13 " and the label, rather than over the label
   itself. Writing it out here keeps the shape in one place: every key in
   the handshake comes through this function. */
bool hkdf_expand_label(const u8 secret[SHA256_SIZE], const char *label,
                       const u8 *ctx, u32 ctx_len, u8 *out, u32 out_len);

/* Derive-Secret, which is expand_label over the hash of a transcript. */
bool tls13_derive_secret(const u8 secret[SHA256_SIZE], const char *label,
                         const u8 *transcript_hash, u32 hash_len,
                         u8 out[SHA256_SIZE]);
