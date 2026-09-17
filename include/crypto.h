#pragma once
#include "types.h"

/* The arithmetic a wireless password is made of.
 *
 * Joining a protected network is not a matter of sending the password. The
 * password is never sent at all: both sides grind it into a key, prove to
 * each other that they arrived at the same one, and the proof is what goes
 * over the air. So the whole of WPA2 sits on four pieces of arithmetic, and
 * all four are here, written out rather than called for.
 *
 *   SHA-1        the hash everything else is built from
 *   HMAC-SHA1    a hash that takes a key, which is what makes it a proof
 *   PBKDF2       the same thing four thousand times, to make guessing dear
 *   AES          the block cipher the traffic itself is encrypted with
 *
 * None of it is invented here and none of it should be: every one is a
 * published specification with published answers, and the checks in
 * kernel/selftest.c use those answers. Cryptography that has not been made
 * to reproduce a number somebody else arrived at is cryptography nobody
 * should trust, including the person who wrote it.
 */

/* --- SHA-1 ---------------------------------------------------------------- */

#define SHA1_SIZE 20

typedef struct {
    u32 h[5];
    u64 bits;
    u8  buf[64];
    u32 have;
} sha1_t;

void sha1_init(sha1_t *s);
void sha1_update(sha1_t *s, const void *data, u32 len);
void sha1_final(sha1_t *s, u8 out[SHA1_SIZE]);
void sha1(const void *data, u32 len, u8 out[SHA1_SIZE]);

/* --- HMAC-SHA1 ------------------------------------------------------------ */

void hmac_sha1(const u8 *key, u32 key_len,
               const u8 *data, u32 data_len, u8 out[SHA1_SIZE]);

/* The same over several pieces without joining them up first, which is what
   the handshake needs: the thing being signed is a header and a body that
   are never next to each other in memory. */
void hmac_sha1_vector(const u8 *key, u32 key_len,
                      const u8 *const *parts, const u32 *lens, u32 n,
                      u8 out[SHA1_SIZE]);

/* --- PBKDF2-HMAC-SHA1 ----------------------------------------------------- */

/* A passphrase and a salt into as many bytes as asked for. For WPA2 the salt
   is the network's name and the count is 4096, which is the whole of the
   defence: it makes one guess cost four thousand hashes. */
void pbkdf2_sha1(const char *password, const u8 *salt, u32 salt_len,
                 u32 iterations, u8 *out, u32 out_len);

/* --- AES ------------------------------------------------------------------ */

#define AES_BLOCK 16

typedef struct {
    u8  round_key[240];      /* enough for 256 bit keys, which use all of it */
    int rounds;
} aes_t;

/* bits is 128 or 256. Anything else leaves the key unusable and returns
   false, rather than quietly encrypting with rubbish. */
bool aes_set_key(aes_t *a, const u8 *key, u32 bits);
void aes_encrypt_block(const aes_t *a, const u8 in[16], u8 out[16]);
void aes_decrypt_block(const aes_t *a, const u8 in[16], u8 out[16]);

/* RFC 3394, which is how the group key arrives inside the handshake: a key
   wrapped in another key, with eight bytes that say whether it survived.
   False means it did not, which means the password was wrong or somebody is
   interfering, and the two are deliberately indistinguishable. */
bool aes_unwrap_key(const u8 *kek, u32 kek_bits,
                    const u8 *in, u32 in_len, u8 *out);
