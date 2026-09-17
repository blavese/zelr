#pragma once
#include "types.h"

/* Joining a protected network, which is not sending the password.
 *
 * The password never crosses the air. Both sides already know it, so both
 * grind it into the same 32 byte key, and then spend four messages proving
 * to each other that they did, without either one saying what it got. That
 * is the whole of WPA2-PSK, and everything below is one of those steps.
 *
 *   PMK   the password and the network's name, ground together 4096 times.
 *         The name is in there so the same password on two networks is two
 *         different keys, and so a table built for one network is useless
 *         against another.
 *
 *   PTK   the PMK plus both ends' addresses and a random number from each,
 *         which is what makes this session's keys different from the last
 *         one's even though the password has not changed. It is three keys
 *         end to end: one to sign the handshake, one to unwrap the group
 *         key, and one for the traffic.
 *
 *   MIC   a signature on each message. The access point sends one it could
 *         only have computed with the right PMK, so a wrong password is
 *         caught here, by the signature failing rather than by anybody
 *         being told.
 *
 * The addresses and the nonces go in smallest first rather than in the
 * order they arrived, because the two ends have to arrive at the same key
 * and neither is in charge. Getting that ordering wrong gives two keys that
 * are each perfectly good and are not the same, and the failure appears
 * later as traffic that cannot be decrypted.
 */

#define PMK_LEN  32
#define PTK_LEN  48        /* KCK 16, KEK 16, TK 16 */
#define KCK_LEN  16
#define KEK_LEN  16
#define TK_LEN   16
#define NONCE_LEN 32
#define MIC_LEN  16

/* The password and the network name into the key both ends share. Slow on
   purpose: this is four thousand and ninety six rounds of HMAC and is meant
   to be, so it is done once when joining and never per packet. */
void wpa_pmk(const char *passphrase, const char *ssid, u8 pmk[PMK_LEN]);

/* This session's keys. aa is the access point's address, spa is ours. */
void wpa_ptk(const u8 pmk[PMK_LEN], const u8 aa[6], const u8 spa[6],
             const u8 anonce[NONCE_LEN], const u8 snonce[NONCE_LEN],
             u8 ptk[PTK_LEN]);

/* The function underneath both of those, exposed because it is the piece
   with an ordering to get wrong and therefore the piece to check. */
void wpa_prf(const u8 *key, u32 key_len, const char *label,
             const u8 *data, u32 data_len, u8 *out, u32 out_len);

/* Whether an EAPOL-Key frame was signed by somebody holding this key.
 *
 * The signature covers the frame including the space the signature sits in,
 * so that field is zeroed for the calculation and put back afterwards. The
 * frame is not modified. */
bool wpa_check_mic(const u8 kck[KCK_LEN], const u8 *frame, u32 len);

/* Signs one, in place, over the same span. */
void wpa_sign(const u8 kck[KCK_LEN], u8 *frame, u32 len);

/* The group key, which arrives wrapped inside the third message. False when
   it does not come out, which is what a wrong password looks like from
   here. */
bool wpa_unwrap_gtk(const u8 kek[KEK_LEN], const u8 *wrapped, u32 wrapped_len,
                    u8 *out, u32 *out_len);

/* Where things are in an EAPOL-Key frame, which is the same layout however
   it arrived. Offsets rather than a struct: the frame is bytes off a wire
   and a struct over it would be a promise about padding that nothing here
   should make. */
#define EAPOL_MIC_OFFSET   81
#define EAPOL_HEADER_LEN   4
#define EAPOL_MIN_LEN      99
