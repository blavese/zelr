#pragma once
#include "types.h"
#include "crypto.h"

/* What a protected wireless network's traffic is encrypted with: CCMP, which
 * is AES in CCM mode (RFC 3610) with an eight byte MIC and a two byte length,
 * keyed with the temporal key the handshake made (wpa.h, the PTK's TK).
 *
 * Every data frame carries a packet number, six bytes counted up by the
 * sender, in an eight byte header after the 802.11 one. The nonce is built
 * from the frame's priority, its sender's address and that number, so no
 * two frames under one key are ever encrypted alike; the MIC covers the
 * frame's header as well as its body (the AAD), with the parts of the header
 * that may change on the way (a retry bit, the sequence number) taken out.
 * A receiver refuses a frame whose number is not past the last it took, which
 * is what makes replaying one useless.
 */

#define CCMP_HDR_LEN 8
#define CCMP_MIC_LEN 8

/* RFC 3610's CCM with M = 8 and L = 2, as CCMP uses it: a 13 byte nonce, the
   AAD and the message, out into cipher text the message's length and its
   MIC. Decrypting gives false when the MIC is not the one it should be, and
   then what was written to out is not to be used. */
void ccm_encrypt(const aes_t *a, const u8 nonce[13], const u8 *aad, u32 aad_len,
                 const u8 *in, u32 len, u8 *out, u8 mic[CCMP_MIC_LEN]);
bool ccm_decrypt(const aes_t *a, const u8 nonce[13], const u8 *aad, u32 aad_len,
                 const u8 *in, u32 len, u8 *out, const u8 mic[CCMP_MIC_LEN]);

/* A data frame encrypted: hdr is its 802.11 header (hdr_len bytes, which
   says whether it has a fourth address and a QoS field), body its payload.
   out gets the header with the Protected bit set, the CCMP header (pn, key
   id), the encrypted body and the MIC: hdr_len + 8 + len + 8 bytes. */
u32 ccmp_encrypt(const aes_t *tk, u64 pn, int key_id, const u8 *hdr, u32 hdr_len,
                 const u8 *body, u32 len, u8 *out);

/* A protected frame decrypted in place: its body comes out at body_out, how
   long it is the return, or -1 when the MIC is wrong or the frame too short.
   *pn is the frame's packet number, for the caller's replay check. */
int ccmp_decrypt(const aes_t *tk, const u8 *frame, u32 len, u32 hdr_len, u8 *body_out, u64 *pn);

/* The 802.11 header's length: 24, 30 with a fourth address, and 2 more with
   a QoS field. 0 for something too short to be one. */
u32 ccmp_hdr_len(const u8 *frame, u32 len);
