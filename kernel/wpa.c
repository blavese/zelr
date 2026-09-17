/* See include/wpa.h. This is the key arithmetic, not the conversation: it
 * says what the keys are, and something with a radio in front of it decides
 * when to compute them. */
#include "wpa.h"
#include "crypto.h"
#include "string.h"

void wpa_pmk(const char *passphrase, const char *ssid, u8 pmk[PMK_LEN]) {
    u32 n = 0;
    while (ssid[n]) n++;
    pbkdf2_sha1(passphrase, (const u8 *)ssid, n, 4096, pmk, PMK_LEN);
}

/* The expansion the standard defines: HMAC of the label, a zero byte, the
 * data, and a counter, repeated until there are enough bytes.
 *
 * The zero byte matters and is easy to leave out, because it is a
 * terminator for a string that is being treated as bytes everywhere else.
 * The counter is a byte after the data, not before it. Both of those
 * produce a key that is perfectly well formed and wrong. */
void wpa_prf(const u8 *key, u32 key_len, const char *label,
             const u8 *data, u32 data_len, u8 *out, u32 out_len) {
    u32 label_len = 0;
    while (label[label_len]) label_len++;

    u8 counter = 0;
    u8 zero = 0;

    while (out_len) {
        const u8 *parts[4] = { (const u8 *)label, &zero, data, &counter };
        u32 lens[4] = { label_len, 1, data_len, 1 };

        u8 block[SHA1_SIZE];
        hmac_sha1_vector(key, key_len, parts, lens, 4, block);

        u32 take = out_len < SHA1_SIZE ? out_len : SHA1_SIZE;
        memcpy(out, block, take);
        out += take;
        out_len -= take;
        counter++;
    }
}

/* Smallest first. Neither end is in charge, so the ordering cannot depend on
   who sent what: both have to sort the same pair the same way. */
static void append_ordered(u8 *dst, const u8 *a, const u8 *b, u32 len) {
    int cmp = memcmp(a, b, len);
    const u8 *first = cmp <= 0 ? a : b;
    const u8 *second = cmp <= 0 ? b : a;
    memcpy(dst, first, len);
    memcpy(dst + len, second, len);
}

void wpa_ptk(const u8 pmk[PMK_LEN], const u8 aa[6], const u8 spa[6],
             const u8 anonce[NONCE_LEN], const u8 snonce[NONCE_LEN],
             u8 ptk[PTK_LEN]) {
    u8 data[6 + 6 + NONCE_LEN + NONCE_LEN];

    append_ordered(data, aa, spa, 6);
    append_ordered(data + 12, anonce, snonce, NONCE_LEN);

    wpa_prf(pmk, PMK_LEN, "Pairwise key expansion",
            data, sizeof(data), ptk, PTK_LEN);
}

/* --- signing a message ---------------------------------------------------- */

/* The signature covers the field it sits in, so that field is zero while it
   is being computed. Copying the frame rather than clearing the field in
   place, because checking a signature must not modify what it was handed:
   the caller may well want to look at it afterwards, and a checker with a
   side effect is a checker that cannot be run twice. */
static void mic_of(const u8 kck[KCK_LEN], const u8 *frame, u32 len,
                   u8 out[MIC_LEN]) {
    u8 copy[256];
    u32 n = len < sizeof(copy) ? len : sizeof(copy);

    memcpy(copy, frame, n);
    if (n >= EAPOL_MIC_OFFSET + MIC_LEN)
        memset(copy + EAPOL_MIC_OFFSET, 0, MIC_LEN);

    u8 full[SHA1_SIZE];
    hmac_sha1(kck, KCK_LEN, copy, n, full);
    memcpy(out, full, MIC_LEN);          /* the first 16 of 20 */
}

bool wpa_check_mic(const u8 kck[KCK_LEN], const u8 *frame, u32 len) {
    if (len < EAPOL_MIN_LEN) return false;

    u8 want[MIC_LEN];
    mic_of(kck, frame, len, want);

    /* Every byte compared whatever the first one said. A comparison that
       stops early tells anyone who can time it how much of a guess was
       right, and that turns a forgery from impossible into tedious. */
    u8 diff = 0;
    for (u32 i = 0; i < MIC_LEN; i++)
        diff |= (u8)(want[i] ^ frame[EAPOL_MIC_OFFSET + i]);
    return diff == 0;
}

void wpa_sign(const u8 kck[KCK_LEN], u8 *frame, u32 len) {
    if (len < EAPOL_MIN_LEN) return;
    u8 mic[MIC_LEN];
    mic_of(kck, frame, len, mic);
    memcpy(frame + EAPOL_MIC_OFFSET, mic, MIC_LEN);
}

/* --- the group key -------------------------------------------------------- */

bool wpa_unwrap_gtk(const u8 kek[KEK_LEN], const u8 *wrapped, u32 wrapped_len,
                    u8 *out, u32 *out_len) {
    if (wrapped_len < 24 || (wrapped_len % 8) != 0) return false;
    if (wrapped_len > 128) return false;

    if (!aes_unwrap_key(kek, KEK_LEN * 8, wrapped, wrapped_len, out))
        return false;

    *out_len = wrapped_len - 8;
    return true;
}
