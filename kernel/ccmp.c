/* See include/ccmp.h. RFC 3610's CCM, and the 802.11 frame around it
 * (IEEE 802.11-2016 12.5.3: the CCMP header, the nonce and the AAD). */
#include "ccmp.h"
#include "string.h"

/* --- CCM ---------------------------------------------------------------------
 *
 * A CBC-MAC over a first block that says what follows (the flags, the nonce
 * and the message's length), the AAD after its own two byte length, and the
 * message, each padded with zeros to a whole block; then counter mode, whose
 * block 0 masks the MAC and blocks 1 on the message. M = 8 is the flags'
 * (M - 2) / 2 = 3 in bits 3 to 5, L = 2 is L - 1 = 1 in the low bits. */

static void ccm_mac(const aes_t *a, const u8 nonce[13], const u8 *aad, u32 aad_len,
                    const u8 *msg, u32 len, u8 t[16]) {
    u8 x[16], b[16];
    b[0] = (u8)((aad_len ? 0x40 : 0) | (3 << 3) | 1);
    memcpy(b + 1, nonce, 13);
    b[14] = (u8)(len >> 8);
    b[15] = (u8)len;
    aes_encrypt_block(a, b, x);
    if (aad_len) {
        /* Its length in two bytes, then the AAD, the blocks filled with
           zeros at the end. */
        u32 at = 0, fill = 2;
        memset(b, 0, 16);
        b[0] = (u8)(aad_len >> 8);
        b[1] = (u8)aad_len;
        while (at < aad_len) {
            while (fill < 16 && at < aad_len) b[fill++] = aad[at++];
            for (int i = 0; i < 16; i++) b[i] ^= x[i];
            aes_encrypt_block(a, b, x);
            memset(b, 0, 16);
            fill = 0;
        }
    }
    for (u32 at = 0; at < len; at += 16) {
        u32 n = len - at < 16 ? len - at : 16;
        for (u32 i = 0; i < 16; i++) b[i] = x[i] ^ (i < n ? msg[at + i] : 0);
        aes_encrypt_block(a, b, x);
    }
    memcpy(t, x, 16);
}

/* Counter block i: flags L - 1, the nonce, the count. */
static void ccm_ctr(const aes_t *a, const u8 nonce[13], u32 i, u8 s[16]) {
    u8 b[16];
    b[0] = 1;
    memcpy(b + 1, nonce, 13);
    b[14] = (u8)(i >> 8);
    b[15] = (u8)i;
    aes_encrypt_block(a, b, s);
}

static void ccm_crypt(const aes_t *a, const u8 nonce[13], const u8 *in, u32 len, u8 *out) {
    u8 s[16];
    for (u32 at = 0, i = 1; at < len; at += 16, i++) {
        ccm_ctr(a, nonce, i, s);
        u32 n = len - at < 16 ? len - at : 16;
        for (u32 k = 0; k < n; k++) out[at + k] = in[at + k] ^ s[k];
    }
}

void ccm_encrypt(const aes_t *a, const u8 nonce[13], const u8 *aad, u32 aad_len,
                 const u8 *in, u32 len, u8 *out, u8 mic[CCMP_MIC_LEN]) {
    u8 t[16], s0[16];
    ccm_mac(a, nonce, aad, aad_len, in, len, t);
    ccm_ctr(a, nonce, 0, s0);
    for (int i = 0; i < CCMP_MIC_LEN; i++) mic[i] = t[i] ^ s0[i];
    ccm_crypt(a, nonce, in, len, out);
}

bool ccm_decrypt(const aes_t *a, const u8 nonce[13], const u8 *aad, u32 aad_len,
                 const u8 *in, u32 len, u8 *out, const u8 mic[CCMP_MIC_LEN]) {
    u8 t[16], s0[16];
    ccm_crypt(a, nonce, in, len, out);
    ccm_mac(a, nonce, aad, aad_len, out, len, t);
    ccm_ctr(a, nonce, 0, s0);
    /* Every byte compared whatever the first says, so how long the answer
       takes says nothing of where it went wrong. */
    u8 diff = 0;
    for (int i = 0; i < CCMP_MIC_LEN; i++) diff |= (u8)(mic[i] ^ t[i] ^ s0[i]);
    return diff == 0;
}

/* --- the frame ---------------------------------------------------------------- */

u32 ccmp_hdr_len(const u8 *frame, u32 len) {
    if (len < 24) return 0;
    int type = (frame[0] >> 2) & 3, subtype = frame[0] >> 4;
    u32 n = 24;
    if (type == 2) {
        if ((frame[1] & 3) == 3) n += 6;          /* to and from the DS: a fourth address */
        if (subtype & 8) n += 2;                  /* QoS */
    }
    return n <= len ? n : 0;
}

/* The AAD (12.5.3.3.3): the frame control with what may change on the way
   taken out (a data frame's subtype bits but QoS, retry, power management,
   more data, and order on a QoS frame) and Protected set; the three
   addresses; the sequence control's fragment number only; a fourth address;
   the QoS control's priority only. And the nonce (12.5.3.3.4): the
   priority, the sender's address, the packet number high byte first. */
static u32 ccmp_aad_nonce(const u8 *hdr, u32 hdr_len, u64 pn, u8 *aad, u8 nonce[13]) {
    int type = (hdr[0] >> 2) & 3;
    int qos = type == 2 && (hdr[0] & 0x80);
    int a4 = type == 2 && (hdr[1] & 3) == 3;
    u32 n = 0;
    aad[n++] = type == 2 ? (u8)(hdr[0] & 0x8F) : hdr[0];
    u8 fc1 = (u8)(hdr[1] & ~(0x08 | 0x10 | 0x20));
    fc1 |= 0x40;
    if (qos) fc1 &= (u8)~0x80;
    aad[n++] = fc1;
    memcpy(aad + n, hdr + 4, 18);
    n += 18;
    aad[n++] = (u8)(hdr[22] & 0x0F);
    aad[n++] = 0;
    if (a4) { memcpy(aad + n, hdr + 24, 6); n += 6; }
    u8 tid = 0;
    if (qos && hdr_len >= (u32)(a4 ? 32 : 26)) {
        tid = (u8)(hdr[a4 ? 30 : 24] & 0x0F);
        aad[n++] = tid;
        aad[n++] = 0;
    }
    nonce[0] = (u8)(tid | (type == 0 ? 0x10 : 0));
    memcpy(nonce + 1, hdr + 10, 6);
    for (int i = 0; i < 6; i++) nonce[7 + i] = (u8)(pn >> (8 * (5 - i)));
    return n;
}

u32 ccmp_encrypt(const aes_t *tk, u64 pn, int key_id, const u8 *hdr, u32 hdr_len,
                 const u8 *body, u32 len, u8 *out) {
    memcpy(out, hdr, hdr_len);
    out[1] |= 0x40;                               /* Protected */
    u8 *ch = out + hdr_len;
    ch[0] = (u8)pn;
    ch[1] = (u8)(pn >> 8);
    ch[2] = 0;
    ch[3] = (u8)(0x20 | ((key_id & 3) << 6));     /* ExtIV, and which key */
    ch[4] = (u8)(pn >> 16);
    ch[5] = (u8)(pn >> 24);
    ch[6] = (u8)(pn >> 32);
    ch[7] = (u8)(pn >> 40);
    u8 aad[32], nonce[13];
    u32 aad_len = ccmp_aad_nonce(out, hdr_len, pn, aad, nonce);
    ccm_encrypt(tk, nonce, aad, aad_len, body, len, ch + CCMP_HDR_LEN, ch + CCMP_HDR_LEN + len);
    return hdr_len + CCMP_HDR_LEN + len + CCMP_MIC_LEN;
}

int ccmp_decrypt(const aes_t *tk, const u8 *frame, u32 len, u32 hdr_len, u8 *body_out, u64 *pn) {
    if (len < hdr_len + CCMP_HDR_LEN + CCMP_MIC_LEN || !(frame[1] & 0x40)) return -1;
    const u8 *ch = frame + hdr_len;
    if (!(ch[3] & 0x20)) return -1;               /* not CCMP's header */
    u64 p = (u64)ch[0] | (u64)ch[1] << 8 | (u64)ch[4] << 16 | (u64)ch[5] << 24 | (u64)ch[6] << 32 | (u64)ch[7] << 40;
    u8 aad[32], nonce[13];
    u32 aad_len = ccmp_aad_nonce(frame, hdr_len, p, aad, nonce);
    u32 n = len - hdr_len - CCMP_HDR_LEN - CCMP_MIC_LEN;
    if (!ccm_decrypt(tk, nonce, aad, aad_len, ch + CCMP_HDR_LEN, n, body_out, frame + len - CCMP_MIC_LEN)) return -1;
    *pn = p;
    return (int)n;
}
