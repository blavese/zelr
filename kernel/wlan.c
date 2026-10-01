/* See include/wlan.h. A station, frame by frame (IEEE 802.11-2016: 9 for
 * the frames, 11.3 for joining, 12.7 for the 4-way handshake). */
#include "wlan.h"
#include "wpa.h"
#include "ccmp.h"
#include "crypto.h"
#include "rng.h"
#include "string.h"

/* --- what is known ------------------------------------------------------------ */

static const wlan_radio *radio;
static wlan_state_t state = WLAN_IDLE;
static const char *why = "not joined";
static wlan_bss bss[WLAN_BSS_MAX];
static int nbss;
static void (*on_eth)(const u8 *eth, u32 len);
static u32 dropped;

/* The network being joined, and where joining has got to. */
static wlan_bss cur;
static u8 pmk[PMK_LEN], ptk[PTK_LEN], gtk[32];
static int gtk_len, gtk_id;
static aes_t tk_aes, gtk_aes;
static int keys_in, group_in;
static u8 anonce[NONCE_LEN], snonce[NONCE_LEN];
static u64 replay_seen;                    /* the last EAPOL-Key replay counter taken */
static int have_replay;
static u64 tx_pn;
static u64 rx_pn[16], rx_gpn;              /* per priority for the pairwise key; the group key's */
static u16 seq;
static int timer, tries;                   /* polls since the last step; how many times it was tried */
static int scan_ch, scan_dwell;

#define WLAN_RETRY_POLLS  20               /* 200 ms before a request is sent again */
#define WLAN_TRIES        4
#define WLAN_HANDSHAKE_POLLS 300           /* three seconds for the handshake to finish */
#define WLAN_DWELL        12               /* polls a channel is listened on while scanning */

static const u8 OUI[3] = { 0x00, 0x0F, 0xAC };

/* Our RSN element: version 1, CCMP for the group and for us, a pre-shared key. */
static const u8 OUR_RSN[22] = { 0x30, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                                1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0 };

static void fail(const char *w) { state = WLAN_FAILED; why = w; keys_in = group_in = 0; }

void wlan_attach(const wlan_radio *r) {
    radio = r;
    state = WLAN_IDLE;
    why = "not joined";
    nbss = 0;
    keys_in = group_in = 0;
    dropped = 0;
}

wlan_state_t wlan_state(void) { return state; }
const char *wlan_why(void) { return why; }
u32 wlan_dropped(void) { return dropped; }
void wlan_on_eth(void (*fn)(const u8 *, u32)) { on_eth = fn; }

int wlan_networks(wlan_bss *out, int max) {
    int n = nbss < max ? nbss : max;
    for (int i = 0; i < n; i++) out[i] = bss[i];
    return n;
}

/* --- frames out --------------------------------------------------------------- */

static u32 mgmt_head(u8 *f, int subtype, const u8 *da, const u8 *bssid) {
    f[0] = (u8)(subtype << 4);                 /* management */
    f[1] = 0;
    f[2] = f[3] = 0;
    memcpy(f + 4, da, 6);
    memcpy(f + 10, radio->mac, 6);
    memcpy(f + 16, bssid, 6);
    f[22] = (u8)(seq << 4);
    f[23] = (u8)(seq >> 4);
    seq = (u16)((seq + 1) & 0xFFF);
    return 24;
}

static u32 put_ie(u8 *f, u32 n, int id, const void *data, u32 len) {
    f[n++] = (u8)id;
    f[n++] = (u8)len;
    memcpy(f + n, data, len);
    return n + len;
}

/* The rates of 802.11b and g, the first four basic. */
static u32 put_rates(u8 *f, u32 n) {
    static const u8 RATES[8] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    static const u8 EXT[4] = { 0x30, 0x48, 0x60, 0x6C };
    n = put_ie(f, n, 1, RATES, 8);
    return put_ie(f, n, 50, EXT, 4);
}

static void send_probe(void) {
    static const u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    u8 f[128];
    u32 n = mgmt_head(f, 4, bcast, bcast);
    n = put_ie(f, n, 0, "", 0);                /* any network */
    n = put_rates(f, n);
    radio->tx(f, n);
}

static void send_auth(void) {
    u8 f[64];
    u32 n = mgmt_head(f, 11, cur.bssid, cur.bssid);
    f[n++] = 0; f[n++] = 0;                    /* open system */
    f[n++] = 1; f[n++] = 0;                    /* the first of two */
    f[n++] = 0; f[n++] = 0;                    /* status */
    radio->tx(f, n);
}

static void send_assoc(void) {
    u8 f[160];
    u32 n = mgmt_head(f, 0, cur.bssid, cur.bssid);
    u16 cap = 0x0001 | (cur.secure ? 0x0010 : 0);   /* an ESS, and privacy on a protected one */
    f[n++] = (u8)cap; f[n++] = (u8)(cap >> 8);
    f[n++] = 10; f[n++] = 0;                   /* listen interval */
    n = put_ie(f, n, 0, cur.ssid, (u32)strlen(cur.ssid));
    n = put_rates(f, n);
    if (cur.secure) { memcpy(f + n, OUR_RSN, sizeof(OUR_RSN)); n += sizeof(OUR_RSN); }
    radio->tx(f, n);
}

/* A data frame to the access point carrying an Ethernet frame's payload:
   the destination as the third address, the type behind LLC/SNAP;
   encrypted once there is a pairwise key, unless told not to. */
static bool send_data(const u8 *da, u16 type, const u8 *payload, u32 len, int plain) {
    static u8 body[1600], f[1700];
    if (len + 8 > sizeof(body)) return false;
    u8 h[24];
    h[0] = 0x08;                               /* data */
    h[1] = 0x01;                               /* to the DS */
    h[2] = h[3] = 0;
    memcpy(h + 4, cur.bssid, 6);
    memcpy(h + 10, radio->mac, 6);
    memcpy(h + 16, da, 6);
    h[22] = (u8)(seq << 4);
    h[23] = (u8)(seq >> 4);
    seq = (u16)((seq + 1) & 0xFFF);
    body[0] = 0xAA; body[1] = 0xAA; body[2] = 0x03;
    body[3] = body[4] = body[5] = 0;
    body[6] = (u8)(type >> 8);
    body[7] = (u8)type;
    memcpy(body + 8, payload, len);
    u32 n;
    if (keys_in && !plain) {
        n = ccmp_encrypt(&tk_aes, ++tx_pn, 0, h, 24, body, len + 8, f);
    } else {
        memcpy(f, h, 24);
        memcpy(f + 24, body, len + 8);
        n = 24 + len + 8;
    }
    radio->tx(f, n);
    return true;
}

bool wlan_send_eth(const u8 *eth, u32 len) {
    if (state != WLAN_CONNECTED || len < 14) return false;
    return send_data(eth, (u16)(eth[12] << 8 | eth[13]), eth + 14, len - 14, 0);
}

/* --- finding networks ------------------------------------------------------------ */

/* Whether an RSN element offers what this does: CCMP for the group and for
   us, and a pre-shared key. */
static int rsn_ours(const u8 *ie, u32 len) {
    if (len < 2 + 2 + 4 + 2 + 4 + 2 + 4) return 0;
    const u8 *p = ie + 2, *end = ie + 2 + ie[1];
    if (p[0] != 1 || p[1] != 0) return 0;
    p += 2;
    if (memcmp(p, OUI, 3) || p[3] != 4) return 0;          /* the group cipher */
    p += 4;
    u32 n = (u32)p[0] | (u32)p[1] << 8;
    p += 2;
    int ccmp = 0;
    for (u32 i = 0; i < n && p + 4 <= end; i++, p += 4) if (!memcmp(p, OUI, 3) && p[3] == 4) ccmp = 1;
    if (!ccmp || p + 2 > end) return 0;
    n = (u32)p[0] | (u32)p[1] << 8;
    p += 2;
    int psk = 0;
    for (u32 i = 0; i < n && p + 4 <= end; i++, p += 4) if (!memcmp(p, OUI, 3) && p[3] == 2) psk = 1;
    return psk;
}

/* A beacon or a probe response: the network it speaks for, kept or brought
   up to date. */
static void heard_bss(const u8 *f, u32 len, int signal) {
    if (len < 36) return;
    wlan_bss b;
    memset(&b, 0, sizeof(b));
    memcpy(b.bssid, f + 16, 6);
    u16 cap = (u16)(f[34] | f[35] << 8);
    int privacy = (cap & 0x10) != 0;
    b.signal = signal;
    for (u32 at = 36; at + 2 <= len && at + 2 + f[at + 1] <= len; at += 2 + f[at + 1]) {
        int id = f[at], n = f[at + 1];
        const u8 *v = f + at + 2;
        if (id == 0 && n <= WLAN_SSID_MAX) { memcpy(b.ssid, v, (u32)n); b.ssid[n] = 0; }
        else if (id == 3 && n >= 1) b.channel = v[0];
        else if (id == 48 && n + 2 <= (int)sizeof(b.rsn)) {
            memcpy(b.rsn, f + at, (u32)n + 2);
            b.rsn_len = n + 2;
        }
    }
    if (!b.ssid[0]) return;                    /* a hidden network: not offered */
    b.secure = !privacy ? 0 : b.rsn_len && rsn_ours(b.rsn, (u32)b.rsn_len) ? 1 : -1;
    for (int i = 0; i < nbss; i++)
        if (!memcmp(bss[i].bssid, b.bssid, 6)) { bss[i] = b; return; }
    if (nbss < WLAN_BSS_MAX) bss[nbss++] = b;
}

void wlan_scan(void) {
    if (!radio) return;
    nbss = 0;
    state = WLAN_SCANNING;
    why = "looking for networks";
    scan_ch = 1;
    scan_dwell = 0;
    radio->channel(scan_ch);
    send_probe();
}

/* --- joining ------------------------------------------------------------------------ */

int wlan_join(const char *ssid, const char *passphrase) {
    if (!radio) return -1;
    int k = -1;
    for (int i = 0; i < nbss; i++) if (!strcmp(bss[i].ssid, ssid)) { if (k < 0 || bss[i].signal > bss[k].signal) k = i; }
    if (k < 0) { why = "no such network was heard"; return -1; }
    if (bss[k].secure < 0) { why = "that network is protected in a way this does not do"; return -1; }
    cur = bss[k];
    keys_in = group_in = 0;
    have_replay = 0;
    tx_pn = 0;
    memset(rx_pn, 0, sizeof(rx_pn));
    rx_gpn = 0;
    if (cur.secure) wpa_pmk(passphrase ? passphrase : "", cur.ssid, pmk);
    radio->channel(cur.channel);
    state = WLAN_AUTH;
    why = "joining";
    timer = 0;
    tries = 1;
    send_auth();
    return 0;
}

void wlan_leave(void) {
    if (radio && (state == WLAN_CONNECTED || state == WLAN_HANDSHAKE || state == WLAN_ASSOC)) {
        u8 f[32];
        u32 n = mgmt_head(f, 12, cur.bssid, cur.bssid);
        f[n++] = 3; f[n++] = 0;                /* leaving */
        radio->tx(f, n);
    }
    state = WLAN_IDLE;
    why = "not joined";
    keys_in = group_in = 0;
}

/* --- the handshake ----------------------------------------------------------------- */

static u16 key_info(const u8 *e) { return (u16)(e[5] << 8 | e[6]); }
static u64 key_replay(const u8 *e) {
    u64 v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | e[9 + i];
    return v;
}

/* An EAPOL-Key frame of ours: the descriptor, its key info, the replay
   counter it answers, a nonce, key data; signed. */
static void send_key(u16 info, u64 replay, const u8 *nonce, const u8 *data, u32 dlen) {
    static u8 e[EAPOL_MIN_LEN + 64];
    memset(e, 0, sizeof(e));
    u32 n = EAPOL_MIN_LEN + dlen;
    e[0] = 2;                                  /* 802.1X-2004 */
    e[1] = 3;                                  /* a key */
    e[2] = (u8)((n - 4) >> 8);
    e[3] = (u8)(n - 4);
    e[4] = 2;                                  /* the RSN descriptor */
    e[5] = (u8)(info >> 8);
    e[6] = (u8)info;
    for (int i = 0; i < 8; i++) e[9 + i] = (u8)(replay >> (56 - 8 * i));
    if (nonce) memcpy(e + 17, nonce, NONCE_LEN);
    e[97] = (u8)(dlen >> 8);
    e[98] = (u8)dlen;
    if (data) memcpy(e + 99, data, dlen);
    wpa_sign(ptk, e, n);
    send_data(cur.bssid, 0x888E, e, n, 1);
}

#define KI_VERSION 2
#define KI_PAIRWISE 0x0008
#define KI_INSTALL  0x0040
#define KI_ACK      0x0080
#define KI_MIC      0x0100
#define KI_SECURE   0x0200
#define KI_ENCRYPTED 0x1000

/* The first message: the access point's nonce; ours made, the keys worked
   out, the second sent with our RSN element. */
static void key_msg1(const u8 *e) {
    memcpy(anonce, e + 17, NONCE_LEN);
    rng_bytes(snonce, NONCE_LEN);
    wpa_ptk(pmk, cur.bssid, radio->mac, anonce, snonce, ptk);
    replay_seen = key_replay(e);
    have_replay = 1;
    send_key(KI_VERSION | KI_PAIRWISE | KI_MIC, replay_seen, snonce, OUR_RSN, sizeof(OUR_RSN));
    timer = 0;
}

/* The third: checked every way it can be, its group key taken out, the
   fourth sent, and the keys put in. */
static void key_msg3(const u8 *e, u32 len) {
    u16 info = key_info(e);
    u64 r = key_replay(e);
    if (!have_replay || r <= replay_seen) { dropped++; return; }             /* replayed */
    if (memcmp(e + 17, anonce, NONCE_LEN)) { dropped++; return; }            /* not the nonce it began with */
    if (!wpa_check_mic(ptk, e, len)) { dropped++; return; }                  /* not from whoever has the key */
    u32 dlen = (u32)(e[97] << 8 | e[98]);
    if (!(info & KI_ENCRYPTED) || EAPOL_MIN_LEN + dlen > len || dlen > 256) { fail("the network's third message was not as it should be"); return; }
    u8 data[256];
    u32 n;
    if (!wpa_unwrap_gtk(ptk + KCK_LEN, e + 99, dlen, data, &n)) { fail("the network's group key would not come out"); return; }
    /* Its RSN element must be the one it said in its beacon, or something
       between us has offered us less. Then the group key's KDE. */
    int rsn_same = 0;
    gtk_len = 0;
    for (u32 at = 0; at + 2 <= n && at + 2 + data[at + 1] <= n; at += 2 + data[at + 1]) {
        const u8 *el = data + at;
        if (el[0] == 48) rsn_same = (u32)el[1] + 2 == (u32)cur.rsn_len && !memcmp(el, cur.rsn, (u32)cur.rsn_len);
        else if (el[0] == 0xDD && el[1] >= 6 && !memcmp(el + 2, OUI, 3) && el[5] == 1 && el[1] - 6 <= 32) {
            gtk_id = el[6] & 3;
            gtk_len = el[1] - 6;
            memcpy(gtk, el + 8, (u32)gtk_len);
        } else if (el[0] == 0xDD && el[1] == 0) break;                         /* padding */
    }
    if (!rsn_same) { fail("the network's handshake did not offer what its beacon did"); return; }
    if (gtk_len != 16) { fail("the network sent no group key this can use"); return; }
    replay_seen = r;
    send_key(KI_VERSION | KI_PAIRWISE | KI_MIC | KI_SECURE, r, 0, 0, 0);
    aes_set_key(&tk_aes, ptk + KCK_LEN + KEK_LEN, 128);
    aes_set_key(&gtk_aes, gtk, 128);
    keys_in = group_in = 1;
    state = WLAN_CONNECTED;
    why = "joined";
}

/* A later group key, as an access point sends when it changes it. */
static void key_group(const u8 *e, u32 len) {
    u64 r = key_replay(e);
    if (r <= replay_seen || !wpa_check_mic(ptk, e, len)) { dropped++; return; }
    u32 dlen = (u32)(e[97] << 8 | e[98]);
    u8 data[256];
    u32 n;
    if (EAPOL_MIN_LEN + dlen > len || dlen > 256 || !wpa_unwrap_gtk(ptk + KCK_LEN, e + 99, dlen, data, &n)) { dropped++; return; }
    for (u32 at = 0; at + 2 <= n && at + 2 + data[at + 1] <= n; at += 2 + data[at + 1]) {
        const u8 *el = data + at;
        if (el[0] == 0xDD && el[1] == 22 && !memcmp(el + 2, OUI, 3) && el[5] == 1) {
            gtk_id = el[6] & 3;
            memcpy(gtk, el + 8, 16);
            aes_set_key(&gtk_aes, gtk, 128);
            rx_gpn = 0;
        }
    }
    replay_seen = r;
    send_key(KI_VERSION | KI_MIC | KI_SECURE, r, 0, 0, 0);
}

static void eapol(const u8 *e, u32 len) {
    if (len < EAPOL_MIN_LEN || e[1] != 3 || e[4] != 2) return;
    u16 info = key_info(e);
    if ((info & 7) != KI_VERSION || !(info & KI_ACK)) return;
    if (info & KI_PAIRWISE) {
        if (!(info & KI_MIC)) { if (state == WLAN_HANDSHAKE || state == WLAN_CONNECTED) key_msg1(e); }
        else if (state == WLAN_HANDSHAKE) key_msg3(e, len);
    } else if (state == WLAN_CONNECTED && (info & KI_MIC)) key_group(e, len);
}

/* --- frames in ------------------------------------------------------------------------- */

static const char *reason(int code) {
    switch (code) {
        case 2:  return "the network no longer knew us";
        case 3:  return "the network sent us away: it is going down";
        case 4:  return "the network let us go for being quiet";
        case 6: case 7: return "the network did not expect what we sent";
        case 8:  return "the network says we left";
        case 15: return "the network gave up on the handshake: the password is not the network's";
        case 23: return "the network refused the keys";
        default: return "the network let us go";
    }
}

static void data_in(const u8 *f, u32 len) {
    u32 hl = ccmp_hdr_len(f, len);
    if (!hl || (f[1] & 3) != 0x02 || memcmp(f + 10, cur.bssid, 6)) return;    /* from our access point to us */
    static u8 body[1700], eth[1700];
    u32 n;
    if (f[1] & 0x40) {
        int group = f[4] & 1;
        if (group ? !group_in : !keys_in) { dropped++; return; }
        u64 pn;
        int got = ccmp_decrypt(group ? &gtk_aes : &tk_aes, f, len, hl, body, &pn);
        if (got < 0) { dropped++; return; }
        int tid = (f[0] & 0x80) ? (f[hl - 2] & 15) : 0;
        u64 *last = group ? &rx_gpn : &rx_pn[tid];
        if (pn <= *last) { dropped++; return; }                               /* seen before */
        *last = pn;
        n = (u32)got;
    } else {
        n = len - hl;
        memcpy(body, f + hl, n);
    }
    if (n < 8 || body[0] != 0xAA || body[1] != 0xAA || body[2] != 0x03) return;
    u16 type = (u16)(body[6] << 8 | body[7]);
    if (type == 0x888E) { eapol(body + 8, n - 8); return; }
    if (!(f[1] & 0x40) && keys_in) { dropped++; return; }                     /* in the clear once there are keys */
    if (state != WLAN_CONNECTED) return;
    memcpy(eth, f + 4, 6);                     /* to: the first address */
    memcpy(eth + 6, f + 16, 6);                /* from: the third */
    memcpy(eth + 12, body + 6, n - 6);
    if (on_eth) on_eth(eth, n - 6 + 12);
}

void wlan_rx(const u8 *f, u32 len, int signal) {
    if (!radio || len < 24) return;
    int type = (f[0] >> 2) & 3, sub = f[0] >> 4;
    if (type == 2) { data_in(f, len); return; }
    if (type != 0) return;
    int to_us = !memcmp(f + 4, radio->mac, 6);
    int from_cur = !memcmp(f + 10, cur.bssid, 6);
    switch (sub) {
        case 8: case 5:                        /* beacon, probe response */
            heard_bss(f, len, signal);
            break;
        case 11:                               /* authentication */
            if (state == WLAN_AUTH && to_us && from_cur && len >= 30 && f[26] == 2) {
                int status = f[28] | f[29] << 8;
                if (status) { fail("the network would not let us in"); break; }
                state = WLAN_ASSOC;
                timer = 0;
                tries = 1;
                send_assoc();
            }
            break;
        case 1:                                /* association response */
            if (state == WLAN_ASSOC && to_us && from_cur && len >= 30) {
                int status = f[26] | f[27] << 8;
                if (status) { fail("the network would not take us"); break; }
                timer = 0;
                if (cur.secure) { state = WLAN_HANDSHAKE; why = "agreeing on keys"; }
                else { state = WLAN_CONNECTED; why = "joined"; }
            }
            break;
        case 10: case 12:                      /* disassociation, deauthentication */
            if (to_us && from_cur && len >= 26 && state != WLAN_IDLE && state != WLAN_SCANNING) fail(reason(f[24] | f[25] << 8));
            break;
    }
}

/* --- time --------------------------------------------------------------------------------- */

void wlan_poll(void) {
    if (!radio) return;
    timer++;
    switch (state) {
        case WLAN_SCANNING:
            if (++scan_dwell < WLAN_DWELL) break;
            scan_dwell = 0;
            if (++scan_ch > 13) { state = WLAN_IDLE; why = "not joined"; break; }
            radio->channel(scan_ch);
            send_probe();
            break;
        case WLAN_AUTH: case WLAN_ASSOC:
            if (timer < WLAN_RETRY_POLLS) break;
            if (++tries > WLAN_TRIES) { fail(state == WLAN_AUTH ? "the network did not answer" : "the network did not take us"); break; }
            timer = 0;
            if (state == WLAN_AUTH) send_auth(); else send_assoc();
            break;
        case WLAN_HANDSHAKE:
            if (timer > WLAN_HANDSHAKE_POLLS) fail("the network did not finish agreeing on keys: a wrong password looks like this");
            break;
        default:
            break;
    }
}
