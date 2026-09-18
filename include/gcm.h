#pragma once
#include "types.h"
#include "crypto.h"

/* AES-GCM, the one cipher every TLS 1.3 implementation is required to have.
 *
 * The block cipher itself is already here, written for WPA2. GCM is what
 * turns it into something that both hides a message and detects a changed
 * one: a counter mode for the hiding, and a hash over the ciphertext in a
 * finite field for the detecting. Getting the second part right is the
 * whole point, because a cipher that encrypts correctly and authenticates
 * incorrectly looks exactly like one that works. */

#define GCM_TAG 16
#define GCM_IV  12

typedef struct {
    aes_t aes;
    u8    h[16];          /* the subkey the field multiplication is by */
} gcm_t;

bool gcm_init(gcm_t *g, const u8 *key, u32 bits);

/* Encrypts in place and writes the tag out. The nonce must never repeat
   under one key, which in TLS is arranged by counting records. */
void gcm_seal(const gcm_t *g, const u8 iv[GCM_IV],
              const u8 *aad, u32 aad_len,
              u8 *data, u32 len, u8 tag[GCM_TAG]);

/* Decrypts in place, and returns whether the tag was right. False means the
   message was altered, and the plaintext is not to be used: on a false the
   buffer is wiped rather than left holding something that looks like data. */
bool gcm_open(const gcm_t *g, const u8 iv[GCM_IV],
              const u8 *aad, u32 aad_len,
              u8 *data, u32 len, const u8 tag[GCM_TAG]);
