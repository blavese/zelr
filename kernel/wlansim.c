/* A simulated access point, for the self test to join: a radio (wlan.h)
 * whose far end is a WPA2 network on channel 6, written from the
 * authenticator's side of IEEE 802.11-2016 12.7 rather than borrowed from the
 * station's, so the two can disagree.
 *
 * It answers probe requests and sends beacons, authenticates and associates
 * the one station there is, runs the 4-way handshake (the first message, the
 * second's MIC and RSN element checked, the third with the group key wrapped
 * in a KDE, the fourth checked) and gives up with reason 15 when the second
 * message's MIC is wrong, as an access point does for a wrong password. Once
 * the keys are in it answers ARP and ping for its own address, encrypting
 * with CCMP, and the test can make it send to everyone, send the same frame
 * twice, or bend one. Frames to the station are queued and delivered by
 * wlansim_pump, never from inside the station's own sending. Run as a
 * machine's radio (the "wlansim" boot word, main.c), it also hands out an
 * address by DHCP (10.77.0.2, itself the router and the name server) and
 * sends a beacon every tenth time it is asked for what it has.
 */
#include "wlansim.h"
#include "wpa.h"
#include "ccmp.h"
#include "crypto.h"
#include "rng.h"
#include "string.h"

static const u8 BSSID[6] = { 0x02, 0x5A, 0x45, 0x4C, 0x52, 0x01 };
static const u8 AP_IP[4] = { 10, 77, 0, 1 };
static const char SSID[] = "zelr-sim";
static const char PASS[] = "only the test knows";
static const u8 AP_RSN[22] = { 0x30, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                               1, 0, 0x00, 0x0F, 0xAC, 2, 0x0C, 0 };   /* capabilities its own */
#define CHANNEL 6

static wlan_radio sim_radio;
static int sta_channel;
static u8 sta[6];
static u8 sta_rsn[64];
static int sta_rsn_len, associated, keys;
static u8 pmk[PMK_LEN], ptk[PTK_LEN], gtk[16], anonce[NONCE_LEN];
static aes_t tk, gk;
static u64 replay, tx_pn, gtx_pn, rx_pn;
static u16 seq;
static int mic_failures, msg4_ok;
static int misbehave;                      /* how the next third message is wrong (wlansim_misbehave) */

/* Frames waiting for the station. */
#define QN 16
static u8 q[QN][1700];
static u32 qlen[QN];
static int qn;
static u8 last_sent[1700];
static u32 last_len;

static void queue(const u8 *f, u32 n) {
    if (qn >= QN || n > sizeof(q[0])) return;
    memcpy(q[qn], f, n);
    qlen[qn++] = n;
    memcpy(last_sent, f, n);
    last_len = n;
}

static u32 head(u8 *f, int type_sub, int flags, const u8 *a1, const u8 *a2, const u8 *a3) {
    f[0] = (u8)type_sub;
    f[1] = (u8)flags;
    f[2] = f[3] = 0;
    memcpy(f + 4, a1, 6);
    memcpy(f + 10, a2, 6);
    memcpy(f + 16, a3, 6);
    f[22] = (u8)(seq << 4);
    f[23] = (u8)(seq >> 4);
    seq = (u16)((seq + 1) & 0xFFF);
    return 24;
}

static u32 ie(u8 *f, u32 n, int id, const void *v, u32 len) {
    f[n++] = (u8)id;
    f[n++] = (u8)len;
    memcpy(f + n, v, len);
    return n + len;
}

/* A beacon (to all) or a probe response (to the station). */
static void advertise(int beacon) {
    static const u8 all[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const u8 rates[8] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    u8 f[200], ch = CHANNEL;
    u32 n = head(f, beacon ? 0x80 : 0x50, 0, beacon ? all : sta, BSSID, BSSID);
    memset(f + n, 0, 8);                       /* timestamp */
    n += 8;
    f[n++] = 100; f[n++] = 0;                  /* beacon interval */
    f[n++] = 0x11; f[n++] = 0;                 /* an ESS, with privacy */
    n = ie(f, n, 0, SSID, sizeof(SSID) - 1);
    n = ie(f, n, 1, rates, 8);
    n = ie(f, n, 3, &ch, 1);
    memcpy(f + n, AP_RSN, sizeof(AP_RSN));
    n += sizeof(AP_RSN);
    queue(f, n);
}

/* A data frame from the network to the station (or to all): LLC/SNAP and
   the type, encrypted when there are keys and it is not told otherwise. */
static void to_sta(const u8 *da, const u8 *sa, u16 type, const u8 *payload, u32 len, int plain) {
    static u8 body[1600], f[1700];
    u8 h[24];
    head(h, 0x08, 0x02, da, BSSID, sa);
    body[0] = 0xAA; body[1] = 0xAA; body[2] = 0x03; body[3] = body[4] = body[5] = 0;
    body[6] = (u8)(type >> 8);
    body[7] = (u8)type;
    memcpy(body + 8, payload, len);
    u32 n;
    if (keys && !plain) {
        int group = da[0] & 1;
        n = ccmp_encrypt(group ? &gk : &tk, group ? ++gtx_pn : ++tx_pn, group ? 1 : 0, h, 24, body, len + 8, f);
    } else {
        memcpy(f, h, 24);
        memcpy(f + 24, body, len + 8);
        n = 24 + len + 8;
    }
    queue(f, n);
}

/* --- the authenticator's handshake ---------------------------------------------- */

static void key_frame(u16 info, const u8 *nonce, const u8 *data, u32 dlen, int sign) {
    static u8 e[400];
    memset(e, 0, sizeof(e));
    u32 n = EAPOL_MIN_LEN + dlen;
    e[0] = 2; e[1] = 3;
    e[2] = (u8)((n - 4) >> 8); e[3] = (u8)(n - 4);
    e[4] = 2;
    e[5] = (u8)(info >> 8); e[6] = (u8)info;
    e[7] = 0; e[8] = 16;                       /* a CCMP key's length */
    if (misbehave != 3 || !(info & 0x1000)) replay++;     /* 3: the third with the first's counter */
    for (int i = 0; i < 8; i++) e[9 + i] = (u8)(replay >> (56 - 8 * i));
    if (nonce) memcpy(e + 17, nonce, NONCE_LEN);
    e[97] = (u8)(dlen >> 8); e[98] = (u8)dlen;
    if (data) memcpy(e + 99, data, dlen);
    if (sign) wpa_sign(ptk, e, n);
    to_sta(sta, BSSID, 0x888E, e, n, 1);
}

static void deauth(int code) {
    u8 f[32];
    u32 n = head(f, 0xC0, 0, sta, BSSID, BSSID);
    f[n++] = (u8)code; f[n++] = 0;
    queue(f, n);
    associated = keys = 0;
}

/* The second message: its MIC under the keys its nonce makes, its RSN
   element the one the station associated with; then the third. */
static void msg2(const u8 *e, u32 len) {
    u8 snonce[NONCE_LEN];
    memcpy(snonce, e + 17, NONCE_LEN);
    wpa_ptk(pmk, BSSID, sta, anonce, snonce, ptk);
    if (!wpa_check_mic(ptk, e, len)) { mic_failures++; deauth(15); return; }
    u32 dlen = (u32)(e[97] << 8 | e[98]);
    if (dlen != (u32)sta_rsn_len || memcmp(e + 99, sta_rsn, dlen)) { deauth(17); return; }
    /* The key data: our RSN element, the group key's KDE, padding to a
       whole eight bytes; wrapped in the KEK. */
    u8 plain[96], wrapped[104];
    u32 n = 0;
    memcpy(plain, AP_RSN, sizeof(AP_RSN));
    if (misbehave == 1) plain[13] = 2;         /* 1: TKIP where the beacon said CCMP */
    n += sizeof(AP_RSN);
    plain[n++] = 0xDD; plain[n++] = 22;
    plain[n++] = 0x00; plain[n++] = 0x0F; plain[n++] = 0xAC; plain[n++] = 1;
    plain[n++] = 1;                            /* key 1 */
    plain[n++] = 0;
    memcpy(plain + n, gtk, 16);
    n += 16;
    if (n % 8) { plain[n++] = 0xDD; while (n % 8) plain[n++] = 0; }
    aes_wrap_key(ptk + KCK_LEN, 128, plain, n, wrapped);
    u8 other[NONCE_LEN];
    memcpy(other, anonce, NONCE_LEN);
    if (misbehave == 2) other[0] ^= 1;         /* 2: another nonce than the first's */
    key_frame(2 | 0x0008 | 0x0040 | 0x0080 | 0x0100 | 0x0200 | 0x1000, other, wrapped, n + 8, 1);
}

static void msg4(const u8 *e, u32 len) {
    u64 r = 0;
    for (int i = 0; i < 8; i++) r = r << 8 | e[9 + i];
    if (r != replay || !wpa_check_mic(ptk, e, len)) { mic_failures++; return; }
    aes_set_key(&tk, ptk + KCK_LEN + KEK_LEN, 128);
    aes_set_key(&gk, gtk, 128);
    keys = 1;
    msg4_ok = 1;
}

/* --- what the station sends ---------------------------------------------------------- */

static void dhcp_answer(const u8 *ip, u32 len);

static void from_sta_data(const u8 *f, u32 len) {
    u32 hl = ccmp_hdr_len(f, len);
    if (!hl || (f[1] & 3) != 0x01) return;
    static u8 body[1700];
    u32 n;
    if (f[1] & 0x40) {
        if (!keys) return;
        u64 pn;
        int got = ccmp_decrypt(&tk, f, len, hl, body, &pn);
        if (got < 0 || pn <= rx_pn) return;
        rx_pn = pn;
        n = (u32)got;
    } else {
        n = len - hl;
        memcpy(body, f + hl, n);
    }
    if (n < 8) return;
    u16 type = (u16)(body[6] << 8 | body[7]);
    const u8 *p = body + 8;
    u32 pl = n - 8;
    if (type == 0x888E && pl >= EAPOL_MIN_LEN) {
        u16 info = (u16)(p[5] << 8 | p[6]);
        if ((info & 0x0108) == 0x0108 && !(info & 0x0200)) msg2(p, pl);
        else if ((info & 0x0308) == 0x0308) msg4(p, pl);
        return;
    }
    if (!keys || !(f[1] & 0x40)) return;                  /* nothing in the clear after the keys */
    if (type == 0x0806 && pl >= 28 && p[7] == 1 && !memcmp(p + 24, AP_IP, 4)) {
        /* Who has our address: we do. */
        u8 r[28];
        memcpy(r, p, 6);
        r[6] = 0; r[7] = 2;
        memcpy(r + 8, BSSID, 6);
        memcpy(r + 14, AP_IP, 4);
        memcpy(r + 18, p + 8, 10);
        to_sta(f + 10, BSSID, 0x0806, r, 28, 0);
    } else if (type == 0x0800 && pl >= 28 + 240 && p[9] == 17 && p[(p[0] & 15) * 4 + 3] == 67) {
        dhcp_answer(p, pl);
    } else if (type == 0x0800 && pl >= 28 && p[9] == 1 && !memcmp(p + 16, AP_IP, 4) && p[20] == 8) {
        /* A ping for us: the same back, as a reply, its sums worked again. */
        static u8 r[1600];
        u32 ihl = (u32)(p[0] & 15) * 4;
        memcpy(r, p, pl);
        memcpy(r + 12, p + 16, 4);
        memcpy(r + 16, p + 12, 4);
        r[ihl] = 0;
        r[ihl + 2] = r[ihl + 3] = 0;
        u32 s = 0;
        for (u32 i = ihl; i + 1 < pl; i += 2) s += (u32)(r[i] << 8 | r[i + 1]);
        if ((pl - ihl) & 1) s += (u32)r[pl - 1] << 8;
        while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
        s = ~s & 0xFFFF;
        r[ihl + 2] = (u8)(s >> 8);
        r[ihl + 3] = (u8)s;
        to_sta(f + 10, BSSID, 0x0800, r, pl, 0);
    }
}

static void from_sta(const u8 *f, u32 len) {
    if (len < 24 || sta_channel != CHANNEL) return;       /* not on our channel: not heard */
    int type = (f[0] >> 2) & 3, sub = f[0] >> 4;
    if (type == 2) { if (!memcmp(f + 4, BSSID, 6)) from_sta_data(f, len); return; }
    if (type != 0) return;
    if (sub == 4) { memcpy(sta, f + 10, 6); advertise(0); return; }       /* a probe */
    if (memcmp(f + 4, BSSID, 6)) return;
    if (sub == 11 && len >= 30 && f[24] == 0 && f[26] == 1) {             /* open authentication, first */
        memcpy(sta, f + 10, 6);
        u8 r[32];
        u32 n = head(r, 0xB0, 0, sta, BSSID, BSSID);
        r[n++] = 0; r[n++] = 0; r[n++] = 2; r[n++] = 0; r[n++] = 0; r[n++] = 0;
        queue(r, n);
    } else if (sub == 0 && len >= 28) {                                    /* association */
        sta_rsn_len = 0;
        for (u32 at = 28; at + 2 <= len && at + 2 + f[at + 1] <= len; at += 2 + f[at + 1])
            if (f[at] == 48 && f[at + 1] + 2 <= (int)sizeof(sta_rsn)) {
                sta_rsn_len = f[at + 1] + 2;
                memcpy(sta_rsn, f + at, (u32)sta_rsn_len);
            }
        u8 r[64];
        u32 n = head(r, 0x10, 0, sta, BSSID, BSSID);
        r[n++] = 0x11; r[n++] = 0;
        r[n++] = sta_rsn_len ? 0 : 40; r[n++] = 0;   /* refused without an RSN element (40: bad element) */
        r[n++] = 0x01; r[n++] = 0xC0;
        queue(r, n);
        if (!sta_rsn_len) return;
        associated = 1;
        keys = 0;
        replay = 0;
        tx_pn = gtx_pn = rx_pn = 0;
        rng_bytes(anonce, NONCE_LEN);
        key_frame(2 | 0x0008 | 0x0080, anonce, 0, 0, 0);
    } else if (sub == 12 || sub == 10) {
        associated = keys = 0;
    }
}

/* --- DHCP: one address, to the one station ------------------------------------------ */

static u16 sum_of(const u8 *p, u32 n, u32 start) {
    u32 s = start;
    for (u32 i = 0; i + 1 < n; i += 2) s += (u32)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (u32)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}

/* A DISCOVER answered with an OFFER, a REQUEST with an ACK: to the station,
   from 10.77.0.1, with the mask, the router and the name server. */
static void dhcp_answer(const u8 *ip, u32 len) {
    u32 ihl = (u32)(ip[0] & 15) * 4;
    if (len < ihl + 8 + 240) return;
    const u8 *b = ip + ihl + 8;
    u32 blen = len - ihl - 8;
    if (b[0] != 1 || b[236] != 99 || b[237] != 130 || b[238] != 83 || b[239] != 99) return;
    int type = 0;
    for (u32 at = 240; at + 2 <= blen && b[at] != 255; at += b[at] == 0 ? 1 : 2 + b[at + 1])
        if (b[at] == 53 && b[at + 1] == 1) type = b[at + 2];
    if (type != 1 && type != 3) return;
    static u8 pkt[20 + 8 + 300];
    memset(pkt, 0, sizeof(pkt));
    u8 *r = pkt + 28;
    r[0] = 2; r[1] = 1; r[2] = 6;
    memcpy(r + 4, b + 4, 4);                   /* the station's transaction */
    r[16] = 10; r[17] = 77; r[18] = 0; r[19] = 2;            /* yours */
    memcpy(r + 20, AP_IP, 4);                  /* the server */
    memcpy(r + 28, b + 28, 16);                /* its hardware address */
    r[236] = 99; r[237] = 130; r[238] = 83; r[239] = 99;
    u32 o = 240;
    r[o++] = 53; r[o++] = 1; r[o++] = type == 1 ? 2 : 5;     /* offer, or ack */
    r[o++] = 54; r[o++] = 4; memcpy(r + o, AP_IP, 4); o += 4;
    r[o++] = 51; r[o++] = 4; r[o++] = 0; r[o++] = 0; r[o++] = 0x0E; r[o++] = 0x10;
    r[o++] = 1;  r[o++] = 4; r[o++] = 255; r[o++] = 255; r[o++] = 255; r[o++] = 0;
    r[o++] = 3;  r[o++] = 4; memcpy(r + o, AP_IP, 4); o += 4;
    r[o++] = 6;  r[o++] = 4; memcpy(r + o, AP_IP, 4); o += 4;
    r[o++] = 255;
    u32 ulen = 8 + o, tlen = 20 + ulen;
    u8 *h = pkt;
    h[0] = 0x45; h[2] = (u8)(tlen >> 8); h[3] = (u8)tlen; h[8] = 64; h[9] = 17;
    memcpy(h + 12, AP_IP, 4);
    memset(h + 16, 255, 4);
    u16 hs = sum_of(h, 20, 0);
    h[10] = (u8)(hs >> 8); h[11] = (u8)hs;
    u8 *u = pkt + 20;
    u[0] = 0; u[1] = 67; u[2] = 0; u[3] = 68;
    u[4] = (u8)(ulen >> 8); u[5] = (u8)ulen;   /* no UDP sum: allowed over IPv4 */
    to_sta(sta, BSSID, 0x0800, pkt, tlen, 0);
}

static void sim_channel(int ch) { sta_channel = ch; }

static int pumps;
static void sim_poll(void) {
    /* A beacon now and then, as an access point sends ten a second. */
    if (++pumps % 10 == 0) advertise(1);
    wlansim_pump();
}

const wlan_radio *wlansim_reset(const u8 mac[6]) {
    sim_radio.tx = from_sta;
    sim_radio.channel = sim_channel;
    sim_radio.poll = sim_poll;
    sim_radio.name = "simulated wireless";
    memcpy(sim_radio.mac, mac, 6);
    wpa_pmk(PASS, SSID, pmk);
    rng_bytes(gtk, sizeof(gtk));
    associated = keys = 0;
    mic_failures = msg4_ok = 0;
    misbehave = 0;
    qn = 0;
    last_len = 0;
    sta_channel = 0;
    seq = 0;
    return &sim_radio;
}

const char *wlansim_ssid(void) { return SSID; }
const char *wlansim_password(void) { return PASS; }
const u8 *wlansim_bssid(void) { return BSSID; }
int wlansim_mic_failures(void) { return mic_failures; }
int wlansim_keys(void) { return keys && msg4_ok; }

/* What is waiting delivered to the station, one frame at a time (each may
   make more). */
void wlansim_pump(void) {
    for (int guard = 0; guard < 64 && qn > 0; guard++) {
        static u8 f[1700];
        u32 n = qlen[0];
        memcpy(f, q[0], n);
        for (int i = 1; i < qn; i++) { memcpy(q[i - 1], q[i], qlen[i]); qlen[i - 1] = qlen[i]; }
        qn--;
        if (sta_channel == CHANNEL) wlan_rx(f, n, -40);
    }
}

void wlansim_beacon(void) { advertise(1); }

/* A frame to everyone, under the group key: an ARP request from the
   network's side. */
void wlansim_broadcast(void) {
    static const u8 all[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    u8 a[28] = { 0, 1, 8, 0, 6, 4, 0, 1 };
    memcpy(a + 8, BSSID, 6);
    memcpy(a + 14, AP_IP, 4);
    memset(a + 18, 0, 6);
    a[24] = 10; a[25] = 77; a[26] = 0; a[27] = 9;
    to_sta(all, BSSID, 0x0806, a, 28, 0);
}

/* The last frame sent, sent again as it was. */
void wlansim_replay(void) { if (last_len) queue(last_sent, last_len); }

/* The last frame sent again, with a byte of its body changed. */
void wlansim_bend(void) {
    if (!last_len) return;
    u8 f[1700];
    memcpy(f, last_sent, last_len);
    f[last_len - 12] ^= 0x20;
    queue(f, last_len);
}

void wlansim_deauth(int code) { deauth(code); }

void wlansim_misbehave(int how) { misbehave = how; }

/* A frame in the clear to the station, as nothing should send once the
   keys are in. */
void wlansim_plain(void) {
    u8 a[28] = { 0, 1, 8, 0, 6, 4, 0, 2 };
    memcpy(a + 8, BSSID, 6);
    memcpy(a + 14, AP_IP, 4);
    memcpy(a + 18, sta, 6);
    a[24] = 10; a[25] = 77; a[26] = 0; a[27] = 2;
    to_sta(sta, BSSID, 0x0806, a, 28, 1);
}
