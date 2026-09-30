/* TLS 1.3.
 *
 * The shape of it: agree a secret nobody watching can work out, use it to
 * derive keys, and have the server prove with a signature that it is the
 * one named in a certificate that somebody trusted vouched for. Those are
 * three separate things and all three have to hold. A connection with the
 * first two and not the third is encrypted to a stranger.
 *
 * One version and one cipher suite, on purpose. Every attack that has
 * broken this protocol in practice worked by persuading two modern
 * implementations to use an old thing they both still supported, and the
 * defence is not to support it. A server that cannot do 1.3 gets refused
 * rather than accommodated.
 *
 * The transcript is the spine. Every handshake message, in order, is hashed
 * into a running total, and the signature and both finished messages are
 * over that total. It is what stops any of the messages being altered,
 * including the ones sent before there were any keys, so the order of
 * hashing against verifying is exactly as important as the arithmetic. */
#include "tls.h"
#include "tcp.h"
#include "sha256.h"
#include "sha512.h"
#include "gcm.h"
#include "x25519.h"
#include "x509.h"
#include "rsa.h"
#include "ec.h"
#include "rng.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "printf.h"
#include "blackbox.h"

#define REC_CHANGE_CIPHER 20
#define REC_ALERT         21
#define REC_HANDSHAKE     22
#define REC_APPDATA       23

#define HS_CLIENT_HELLO   1
#define HS_SERVER_HELLO   2
#define HS_NEW_TICKET     4
#define HS_ENCRYPTED_EXT  8
#define HS_CERTIFICATE    11
#define HS_CERT_VERIFY    15
#define HS_FINISHED       20
#define HS_KEY_UPDATE     24

#define SUITE_AES128_GCM_SHA256 0x1301
#define GROUP_X25519 0x001d

/* The largest a record may be, plus what the encryption adds. */
#define REC_MAX 16640
/* Handshake messages, chiefly the certificate chain, which is the big one. */
#define HS_MAX  32768

typedef struct {
    bool open;
    bool handshake_done;
    bool ended;

    u8 priv[32], pub[32], peer_share[32];
    u8 client_random[32];
    u8 session_id[32];

    sha256_t transcript;

    u8 hs_secret[32];
    u8 c_hs[32], s_hs[32];
    u8 c_app[32], s_app[32];

    gcm_t c_gcm, s_gcm;
    u8 c_iv[12], s_iv[12];
    u64 c_seq, s_seq;
    bool keys_are_app;

    /* Whether a record of ours can be sealed yet, which decides whether an
       alert goes in the clear or encrypted; and whether one has already gone
       either way, or the server sent its own, after which nothing more is
       said. */
    bool can_seal;
    bool alerted;

    /* Records come in whole; handshake messages do not line up with them,
       so they are reassembled here. */
    u8 *rec;          /* one record's worth */
    u8 *hs;           /* handshake messages being reassembled */
    u32 hs_len, hs_pos;

    /* How much of the record being read has arrived: its header into head,
       then its body into rec. Kept from one read to the next, so a read
       whose time runs out in the middle of a record loses nothing. */
    u8 head[5];
    u32 have;

    /* Decrypted application data waiting to be read. */
    u8 *app;
    u32 app_len, app_pos;

    char host[256];
    char error[128];
    char described[64];

    /* Which connection this session is running over. The stack underneath
       holds several now, so "the connection" is no longer a thing that can
       be assumed: a session has to carry the one it was handed. */
    int  tcp;

    /* The chain the server sent and its first certificate, read.
     *
       These were statics inside tls_connect, shared by every session. A
       handshake waits on the network between receiving the chain and
       checking it, and a wait gives the processor away now, so a second
       handshake could run in that gap and leave the first one checking
       another site's certificates against its own host name. */
    x509_t    leaf;
    const u8 *ders[X509_MAX_CHAIN];
    u32       lens[X509_MAX_CHAIN];
    u8       *chain;          /* the certificates, copied out of the handshake */
} tls_t;

/* One per connection underneath, indexed by the handle the stack gave out.
   A session belongs to exactly one connection and a connection carries at
   most one session, so there is no second handle space to keep in step.

   `nowhere` is where a call with no connection lands: tls_connect(-1) has
   to be able to say why it refused, and the answer has to be somewhere the
   caller can read it back from. */
static tls_t sessions[TCP_MAX];
static tls_t nowhere;

static tls_t *tls_of(int tcp) {
    if (tcp < 0 || tcp >= TCP_MAX) return &nowhere;
    return &sessions[tcp];
}

static void fail(tls_t *z, const char *why) {
    u32 i = 0;
    while (why[i] && i < sizeof(z->error) - 1) { z->error[i] = why[i]; i++; }
    z->error[i] = 0;

    /* And where somebody with no connection can still read it. A handshake
       that fails takes its connection down with it, so asking why by handle
       would be asking about a handle that is already gone -- which is
       exactly when the question gets asked. */
    if (z != &nowhere) memcpy(nowhere.error, z->error, sizeof(nowhere.error));
}

const char *tls_error(int tcp) {
    tls_t *z = tls_of(tcp);
    return z->error[0] ? z->error : "no error";
}

const char *tls_describe(int tcp) {
    tls_t *z = tls_of(tcp);
    return z->described[0] ? z->described : "";
}

bool tls_active(int tcp) { return tls_of(tcp)->open; }

bool tls_ended(int tcp) {
    tls_t *z = tls_of(tcp);
    return z->ended && z->app_pos >= z->app_len;
}

/* Whether anything at all is encrypted just now, which is a question about
   the machine rather than about one connection. */
bool tls_any(void) {
    for (int i = 0; i < TCP_MAX; i++) if (sessions[i].open) return true;
    return false;
}

/* --- reading and writing whole things over TCP --------------------------- */

/* For the self test: bytes to read instead of the network, and a place to
   put what would have been sent, so what this layer does with a record can be
   checked to the byte without a server. */
static const u8 *test_in;
static u32 test_in_len, test_in_pos;
static bool test_in_waits;       /* running out means "not yet", not "ended" */
static u8 *test_out;
static u32 test_out_len, test_out_cap;
static u32 test_writes;          /* how many writes a record took */

u32 tls_test_writes(void) { return test_writes; }

/* Some of what is wanted, waiting up to wait_ms for it: how many bytes
   came, or -1 once the connection has ended. */
static int take_some(tls_t *z, u8 *out, u32 n, u32 wait_ms) {
    if (test_in) {
        u32 left = test_in_len - test_in_pos;
        if (!left) return test_in_waits ? 0 : -1;
        u32 k = left < n ? left : n;
        memcpy(out, test_in + test_in_pos, k);
        test_in_pos += k;
        return (int)k;
    }
    u32 got = tcp_recv(z->tcp, out, n, wait_ms);
    if (got) return (int)got;
    return tcp_ended(z->tcp) ? -1 : 0;
}

/* Reads the record under way until it is whole or the time runs out: 1 once
   it is whole, 0 when the time ran out first (what did arrive is kept for the
   next read), -1 when the connection ended.
 *
 * The header and the body were read with twenty seconds each, whatever the
 * caller had asked for, and what had arrived was thrown away when that ran
 * out. So a program that asked for four seconds could be held for forty, and
 * a server quiet for twenty in the middle of its answer ended the session, and
 * the answer looked finished when it was not. */
static int fill_record(tls_t *z, u32 timeout_ms) {
    u64 deadline = timer_ticks() + (u64)timeout_ms * timer_hz() / 1000;
    for (;;) {
        u8 *to;
        u32 want;
        if (z->have < 5) {
            to = z->head + z->have;
            want = 5 - z->have;
        } else {
            u32 n = ((u32)z->head[3] << 8) | z->head[4];
            if (n == 0 || n > REC_MAX) return 1;      /* read_record refuses it */
            if (z->have - 5 >= n) return 1;
            to = z->rec + (z->have - 5);
            want = n - (z->have - 5);
        }

        /* Past the deadline, what is already here is still taken; only the
           waiting stops. */
        u64 now = timer_ticks();
        u32 wait = 0;
        if (now < deadline) {
            u64 ms = (deadline - now) * 1000 / timer_hz();
            wait = ms > 1000 ? 1000 : (u32)ms;
        }
        int got = take_some(z, to, want, wait);
        if (got < 0) return -1;
        if (got == 0 && (wait == 0 || test_in)) return 0;
        z->have += (u32)got;
    }
}

/* The stack underneath sends at most a segment at a time. */
static bool write_all(tls_t *z, const u8 *p, u32 n) {
    if (test_out) {
        if (test_out_cap - test_out_len < n) return false;
        memcpy(test_out + test_out_len, p, n);
        test_out_len += n;
        test_writes++;
        return true;
    }
    while (n) {
        u16 take = n > 1400 ? 1400 : (u16)n;
        if (!tcp_send(z->tcp, p, take)) return false;
        p += take;
        n -= take;
    }
    return true;
}

/* --- the record layer ----------------------------------------------------- */

/* The nonce is the fixed part exclusive-ored with the record number, which
   is why a number must never repeat under one key. */
static void nonce_for(u8 out[12], const u8 iv[12], u64 seq) {
    memcpy(out, iv, 12);
    for (int i = 0; i < 8; i++)
        out[11 - i] ^= (u8)(seq >> (i * 8));
}

/* A record goes to TCP in one write, its five byte header in front of its
   body. They went as two, and with one segment in flight the header went
   alone and waited for its acknowledgement -- which a server delays, 40 ms
   on Linux and up to 200 elsewhere, in the hope of something to carry it
   with -- before the body could follow: once for every record, the
   ClientHello included. z->rec has room for the header in front.
 *
 * The data may be z->rec itself (the ClientHello is built there), so it is
 * moved rather than copied. */
static bool send_plain_record(tls_t *z, u8 type, const u8 *data, u32 len) {
    if (len > REC_MAX) return false;
    u8 *out = z->rec;
    memmove(out + 5, data, len);
    out[0] = type; out[1] = 0x03; out[2] = 0x03;
    out[3] = (u8)(len >> 8); out[4] = (u8)len;
    return write_all(z, out, len + 5);
}

/* An encrypted record hides what kind of record it is: the real type goes
   inside, after the data, and the outer one always says application data. */
static bool send_encrypted(tls_t *z, u8 inner_type, const u8 *data, u32 len) {
    if (len + 1 + GCM_TAG > REC_MAX) return false;

    u8 *head = z->rec;
    u8 *buf = z->rec + 5;
    memmove(buf, data, len);
    buf[len] = inner_type;
    u32 body = len + 1;

    u32 total = body + GCM_TAG;
    head[0] = REC_APPDATA; head[1] = 0x03; head[2] = 0x03;
    head[3] = (u8)(total >> 8); head[4] = (u8)total;

    u8 nonce[12];
    nonce_for(nonce, z->c_iv, z->c_seq);
    u8 tag[GCM_TAG];
    gcm_seal(&z->c_gcm, nonce, head, 5, buf, body, tag);
    z->c_seq++;

    memcpy(buf + body, tag, GCM_TAG);
    return write_all(z, head, total + 5);
}

/* --- saying why ----------------------------------------------------------
 *
 * A failure used to be kept to ourselves: the connection simply stopped, and
 * the server's log said the client went away. TLS has a way to say why, and a
 * server operator with a certificate that expired yesterday, or a chain with
 * a link missing, is exactly who needs to hear it. One fatal alert, in the
 * clear before this end has a key to seal with and sealed after, and never
 * in answer to one of the server's own. Descriptions are RFC 8446 6.2. */
#define ALERT_UNEXPECTED_MESSAGE   10
#define ALERT_BAD_RECORD_MAC       20
#define ALERT_RECORD_OVERFLOW      22
#define ALERT_HANDSHAKE_FAILURE    40
#define ALERT_BAD_CERTIFICATE      42
#define ALERT_CERTIFICATE_EXPIRED  45
#define ALERT_ILLEGAL_PARAMETER    47
#define ALERT_UNKNOWN_CA           48
#define ALERT_DECODE_ERROR         50
#define ALERT_DECRYPT_ERROR        51
#define ALERT_PROTOCOL_VERSION     70
#define ALERT_INTERNAL_ERROR       80
#define ALERT_MISSING_EXTENSION   109

static void send_alert(tls_t *z, u8 desc) {
    if (z->alerted) return;
    z->alerted = true;
    u8 a[2] = { 2, desc };                     /* fatal */
    if (z->can_seal) send_encrypted(z, REC_ALERT, a, 2);
    else             send_plain_record(z, REC_ALERT, a, 2);
}

/* A failure the server should be told about, as opposed to fail() on its own
   for the ones it cannot be: the connection gone, or its own alert. */
static void refuse(tls_t *z, const char *why, u8 desc) {
    send_alert(z, desc);
    fail(z, why);
}

/* Which alert a certificate chain's verdict is. */
static u8 alert_for_chain(x509_result_t r) {
    if (r == X509_EXPIRED || r == X509_NOT_YET_VALID) return ALERT_CERTIFICATE_EXPIRED;
    if (r == X509_UNTRUSTED) return ALERT_UNKNOWN_CA;
    return ALERT_BAD_CERTIFICATE;
}

/* Reads one record within timeout_ms. When encryption is on, unwraps it and
   reports the type that was inside. 1 for a record; 0 when the time ran out
   before one was whole, which loses nothing (the rest is read next time);
   -1 on a broken connection or a record that does not authenticate, which
   are not distinguished on purpose. */
static int read_record(tls_t *z, u8 *type, u32 *len, bool encrypted, u32 timeout_ms) {
    int r = fill_record(z, timeout_ms);
    if (r < 0) {
        fail(z, z->have < 5 ? "the connection stopped" : "the connection stopped mid record");
        return -1;
    }
    if (r == 0) return 0;

    const u8 *head = z->head;
    u32 n = ((u32)head[3] << 8) | head[4];
    if (n == 0 || n > REC_MAX) { refuse(z, "the server sent a record of an impossible size", ALERT_RECORD_OVERFLOW); return -1; }
    z->have = 0;                         /* taken: the next record starts afresh */

    if (head[0] == REC_CHANGE_CIPHER) {
        /* Meaningless in 1.3 and sent anyway, so that the connection looks
           like an older one to anything in the middle counting records. */
        *type = REC_CHANGE_CIPHER;
        *len = 0;
        return 1;
    }

    if (head[0] == REC_ALERT && !encrypted) {
        z->alerted = true;                     /* not answered */
        fail(z, "the server refused the connection");
        return -1;
    }

    if (!encrypted) {
        *type = head[0];
        *len = n;
        return 1;
    }

    if (n < GCM_TAG + 1) { refuse(z, "the server sent a record too short to be real", ALERT_DECODE_ERROR); return -1; }

    u32 body = n - GCM_TAG;
    u8 nonce[12];
    nonce_for(nonce, z->s_iv, z->s_seq);
    if (!gcm_open(&z->s_gcm, nonce, head, 5, z->rec, body, z->rec + body)) {
        refuse(z, "a record did not authenticate", ALERT_BAD_RECORD_MAC);
        return -1;
    }
    z->s_seq++;

    /* The real type is the last byte that is not padding. */
    while (body > 0 && z->rec[body - 1] == 0) body--;
    if (body == 0) { refuse(z, "a record had nothing in it", ALERT_UNEXPECTED_MESSAGE); return -1; }

    *type = z->rec[body - 1];
    *len = body - 1;

    if (*type == REC_ALERT) {
        /* A close is an alert and so is a refusal; the second byte says. */
        if (*len >= 2 && z->rec[0] == 1) { z->ended = true; *len = 0; return 1; }
        z->alerted = true;
        fail(z, "the server closed the connection with an alert");
        return -1;
    }
    return 1;
}

/* --- handshake message reassembly ---------------------------------------- */

/* Handshake messages do not line up with records: one record can hold
   several, and one message can span several records, and the certificate
   chain is nearly always the second of those. */
static bool hs_fill(tls_t *z, bool encrypted) {
    for (;;) {
        u8 type; u32 len;
        /* Twenty seconds for a record of the handshake, which nobody is
           waiting to read in the middle of. */
        int r = read_record(z, &type, &len, encrypted, 20000);
        if (r == 0) fail(z, z->have < 5 ? "the connection stopped" : "the connection stopped mid record");
        if (r <= 0) return false;
        if (type == REC_CHANGE_CIPHER) continue;
        if (type != REC_HANDSHAKE) {
            refuse(z, "the server sent something other than a handshake message", ALERT_UNEXPECTED_MESSAGE);
            return false;
        }
        if (z->hs_len + len > HS_MAX) { refuse(z, "the handshake is too big to hold", ALERT_INTERNAL_ERROR); return false; }
        memcpy(z->hs + z->hs_len, z->rec, len);
        z->hs_len += len;
        return true;
    }
}

/* One handshake message, with its four byte header, both of which the
   transcript needs. */
static bool hs_next(tls_t *z, bool encrypted, u8 *type, const u8 **body, u32 *len,
                    const u8 **whole, u32 *whole_len) {
    for (;;) {
        u32 avail = z->hs_len - z->hs_pos;
        if (avail >= 4) {
            const u8 *p = z->hs + z->hs_pos;
            u32 n = ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
            if (n > HS_MAX) { refuse(z, "a handshake message claims an impossible size", ALERT_DECODE_ERROR); return false; }
            if (avail >= 4 + n) {
                *type = p[0];
                *body = p + 4;
                *len = n;
                *whole = p;
                *whole_len = 4 + n;
                z->hs_pos += 4 + n;
                return true;
            }
        }
        /* Not a whole message yet. Shuffle what is left to the front so the
           buffer does not creep forward for ever on a long connection. */
        if (z->hs_pos) {
            memmove(z->hs, z->hs + z->hs_pos, z->hs_len - z->hs_pos);
            z->hs_len -= z->hs_pos;
            z->hs_pos = 0;
        }
        if (!hs_fill(z, encrypted)) return false;
    }
}

/* --- the key schedule ----------------------------------------------------- */

static bool set_keys(tls_t *z, const u8 secret[32], bool ours) {
    u8 key[16], iv[12];
    if (!hkdf_expand_label(secret, "key", 0, 0, key, 16)) return false;
    if (!hkdf_expand_label(secret, "iv", 0, 0, iv, 12)) return false;

    if (ours) {
        if (!gcm_init(&z->c_gcm, key, 128)) return false;
        memcpy(z->c_iv, iv, 12);
        z->c_seq = 0;
        z->can_seal = true;
    } else {
        if (!gcm_init(&z->s_gcm, key, 128)) return false;
        memcpy(z->s_iv, iv, 12);
        z->s_seq = 0;
    }
    return true;
}

static void transcript_hash(tls_t *z, u8 out[32]) {
    sha256_t copy = z->transcript;
    sha256_final(&copy, out);
}

/* --- building the client hello -------------------------------------------- */

static void put16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

static u32 build_client_hello(tls_t *z, u8 *out, u32 cap) {
    u32 n = 0;
    if (cap < 512) return 0;

    out[n++] = 0x03; out[n++] = 0x03;          /* what 1.3 has to claim to be */
    memcpy(out + n, z->client_random, 32); n += 32;

    /* A session id, which 1.3 does not use and sends anyway so the
       exchange looks ordinary to anything in the middle. */
    out[n++] = 32;
    memcpy(out + n, z->session_id, 32); n += 32;

    put16(out + n, 2); n += 2;                 /* one cipher suite */
    put16(out + n, SUITE_AES128_GCM_SHA256); n += 2;

    out[n++] = 1; out[n++] = 0;                /* no compression */

    u32 ext_len_at = n; n += 2;
    u32 ext_start = n;

    /* server_name: which site is being asked for, because one address
       serves thousands and the certificate depends on the answer. */
    {
        u32 host_len = 0;
        while (z->host[host_len]) host_len++;
        put16(out + n, 0x0000); n += 2;
        put16(out + n, host_len + 5); n += 2;
        put16(out + n, host_len + 3); n += 2;
        out[n++] = 0;                          /* a host name */
        put16(out + n, host_len); n += 2;
        memcpy(out + n, z->host, host_len); n += host_len;
    }

    /* supported_versions: the real version negotiation in 1.3. */
    put16(out + n, 0x002b); n += 2;
    put16(out + n, 3); n += 2;
    out[n++] = 2;
    put16(out + n, 0x0304); n += 2;

    /* supported_groups */
    put16(out + n, 0x000a); n += 2;
    put16(out + n, 4); n += 2;
    put16(out + n, 2); n += 2;
    put16(out + n, GROUP_X25519); n += 2;

    /* signature_algorithms: what this can actually check. Advertising
       something unverifiable here means a server signs with it and the
       handshake fails at the last step. */
    {
        static const u16 algs[] = {
            0x0403,   /* ecdsa with p-256 and sha-256 */
            0x0804,   /* rsa-pss with sha-256 */
            0x0503,   /* ecdsa with p-384 and sha-384 */
            0x0401    /* rsa pkcs#1 with sha-256, for the certificates */
        };
        u32 count = sizeof(algs) / sizeof(algs[0]);
        put16(out + n, 0x000d); n += 2;
        put16(out + n, count * 2 + 2); n += 2;
        put16(out + n, count * 2); n += 2;
        for (u32 i = 0; i < count; i++) { put16(out + n, algs[i]); n += 2; }
    }

    /* key_share: the public half of a key agreed for this connection only,
       sent with the first message so the whole thing takes one round trip. */
    put16(out + n, 0x0033); n += 2;
    put16(out + n, 32 + 6); n += 2;
    put16(out + n, 32 + 4); n += 2;
    put16(out + n, GROUP_X25519); n += 2;
    put16(out + n, 32); n += 2;
    memcpy(out + n, z->pub, 32); n += 32;

    put16(out + ext_len_at, n - ext_start);
    return n;
}

/* --- reading the server hello --------------------------------------------- */

static bool parse_server_hello(tls_t *z, const u8 *p, u32 len) {
    /* The value a server puts in the random field when it is not really a
       server hello but a request to start again with a different group. */
    static const u8 RETRY[32] = {
        0xcf,0x21,0xad,0x74,0xe5,0x9a,0x61,0x11,0xbe,0x1d,0x8c,0x02,
        0x1e,0x65,0xb8,0x91,0xc2,0xa2,0x11,0x16,0x7a,0xbb,0x8c,0x5e,
        0x07,0x9e,0x09,0xe2,0xc8,0xa8,0x33,0x9c
    };

    if (len < 38) { refuse(z, "the server hello is too short", ALERT_DECODE_ERROR); return false; }
    u32 i = 2;                                  /* the legacy version */

    if (memcmp(p + i, RETRY, 32) == 0) {
        refuse(z, "the server wants a key exchange group this does not have", ALERT_ILLEGAL_PARAMETER);
        return false;
    }
    i += 32;

    u32 sid = p[i++];
    if (i + sid + 3 > len) { refuse(z, "the server hello is malformed", ALERT_DECODE_ERROR); return false; }
    i += sid;

    u32 suite = ((u32)p[i] << 8) | p[i + 1]; i += 2;
    if (suite != SUITE_AES128_GCM_SHA256) {
        refuse(z, "the server chose a cipher this does not have", ALERT_ILLEGAL_PARAMETER);
        return false;
    }
    i++;                                        /* the compression byte */

    if (i + 2 > len) { refuse(z, "the server hello has no extensions", ALERT_DECODE_ERROR); return false; }
    u32 ext_len = ((u32)p[i] << 8) | p[i + 1]; i += 2;
    if (i + ext_len > len) { refuse(z, "the server hello is malformed", ALERT_DECODE_ERROR); return false; }

    bool got_version = false, got_share = false;
    u32 end = i + ext_len;
    while (i + 4 <= end) {
        u32 kind = ((u32)p[i] << 8) | p[i + 1];
        u32 n = ((u32)p[i + 2] << 8) | p[i + 3];
        i += 4;
        if (i + n > end) { refuse(z, "an extension runs past the message", ALERT_DECODE_ERROR); return false; }

        if (kind == 0x002b) {
            if (n != 2) { refuse(z, "the version extension is malformed", ALERT_DECODE_ERROR); return false; }
            u32 v = ((u32)p[i] << 8) | p[i + 1];
            if (v != 0x0304) {
                refuse(z, "the server does not do TLS 1.3", ALERT_PROTOCOL_VERSION);
                return false;
            }
            got_version = true;
        } else if (kind == 0x0033) {
            if (n < 4) { refuse(z, "the key share is malformed", ALERT_DECODE_ERROR); return false; }
            u32 group = ((u32)p[i] << 8) | p[i + 1];
            u32 klen = ((u32)p[i + 2] << 8) | p[i + 3];
            if (group != GROUP_X25519 || klen != 32 || n != klen + 4) {
                refuse(z, "the server chose a key exchange this does not have", ALERT_ILLEGAL_PARAMETER);
                return false;
            }
            memcpy(z->peer_share, p + i + 4, 32);
            got_share = true;
        }
        i += n;
    }

    if (!got_version) {
        /* Without the extension this is an older server pretending, or an
           older server being honest. Either way it is not 1.3. */
        refuse(z, "the server did not agree to TLS 1.3", ALERT_PROTOCOL_VERSION);
        return false;
    }
    if (!got_share) { refuse(z, "the server sent no key share", ALERT_MISSING_EXTENSION); return false; }
    return true;
}

/* --- what the server signs ------------------------------------------------ */

static bool check_cert_verify(tls_t *z, const u8 *p, u32 len, const x509_t *leaf,
                              const u8 hash_before[32]) {
    if (len < 4) { refuse(z, "the signature message is too short", ALERT_DECODE_ERROR); return false; }
    u32 scheme = ((u32)p[0] << 8) | p[1];
    u32 sig_len = ((u32)p[2] << 8) | p[3];
    if (4 + sig_len != len) { refuse(z, "the signature message is malformed", ALERT_DECODE_ERROR); return false; }
    const u8 *sig = p + 4;

    /* What is signed: a long run of spaces, a fixed sentence, a zero, and
       the transcript so far. The preamble is there so that a signature made
       for one purpose cannot be replayed as one made for another. */
    static const char CONTEXT[] = "TLS 1.3, server CertificateVerify";
    u8 pad[64];
    memset(pad, 0x20, sizeof(pad));

    u8 h256[32], h384[48];
    sha256_t s2;
    sha512_t s3;
    u8 zero = 0;

    sha256_init(&s2);
    sha256_update(&s2, pad, 64);
    sha256_update(&s2, CONTEXT, sizeof(CONTEXT) - 1);
    sha256_update(&s2, &zero, 1);
    sha256_update(&s2, hash_before, 32);
    sha256_final(&s2, h256);

    sha384_init(&s3);
    sha512_update(&s3, pad, 64);
    sha512_update(&s3, CONTEXT, sizeof(CONTEXT) - 1);
    sha512_update(&s3, &zero, 1);
    sha512_update(&s3, hash_before, 32);
    sha512_final(&s3, h384);

    switch (scheme) {
        case 0x0403:                            /* ecdsa, p-256, sha-256 */
            if (leaf->key_type != X509_KEY_EC || leaf->curve != EC_P256) break;
            {
                const u8 *r, *s; u32 rl, sl;
                if (!x509_ecdsa_split(sig, sig_len, &r, &rl, &s, &sl)) break;
                if (ec_verify(EC_P256, leaf->ec, leaf->ec_len, h256, 32,
                              r, rl, s, sl)) return true;
            }
            break;

        case 0x0503:                            /* ecdsa, p-384, sha-384 */
            if (leaf->key_type != X509_KEY_EC || leaf->curve != EC_P384) break;
            {
                const u8 *r, *s; u32 rl, sl;
                if (!x509_ecdsa_split(sig, sig_len, &r, &rl, &s, &sl)) break;
                if (ec_verify(EC_P384, leaf->ec, leaf->ec_len, h384, 48,
                              r, rl, s, sl)) return true;
            }
            break;

        case 0x0804:                            /* rsa-pss, sha-256 */
            if (leaf->key_type != X509_KEY_RSA) break;
            if (rsa_verify_pss(&leaf->rsa, sig, sig_len, h256, 32)) return true;
            break;

        default:
            refuse(z, "the server signed with something this cannot check", ALERT_ILLEGAL_PARAMETER);
            return false;
    }

    refuse(z, "the server's signature over the handshake is wrong", ALERT_DECRYPT_ERROR);
    return false;
}

/* --- the order of the server's encrypted messages ------------------------- */

/* One step at a time, each allowing exactly one message: encrypted extensions,
   the certificate, the signature, finished. A function of its own so that the
   self test can hold the rule to account without a server. */
int tls_flight_step(int step, u8 type, const char **why) {
    static const u8 NEXT[] = { HS_ENCRYPTED_EXT, HS_CERTIFICATE, HS_CERT_VERIFY, HS_FINISHED };
    if (step < TLS_FLIGHT_START || step >= TLS_FLIGHT_DONE) {
        if (why) *why = "the server kept talking after its handshake was over";
        return -1;
    }
    if (type != NEXT[step]) {
        /* Named apart from any other slip, because it is the one that
           matters: a finished message with no signature before it is a
           server that has not shown it holds the certificate's key. */
        if (why) *why = type == HS_FINISHED
                          ? "the server never proved it holds the certificate's key"
                          : "the server sent its handshake out of order";
        return -1;
    }
    return step + 1;
}

/* --- the handshake -------------------------------------------------------- */

static bool alloc_buffers(tls_t *z) {
    if (!z->rec)   z->rec   = (u8 *)kmalloc(REC_MAX + 64);
    if (!z->hs)    z->hs    = (u8 *)kmalloc(HS_MAX);
    if (!z->app)   z->app   = (u8 *)kmalloc(REC_MAX);
    if (!z->chain) z->chain = (u8 *)kmalloc(HS_MAX);
    return z->rec && z->hs && z->app && z->chain;
}

bool tls_connect(int tcp, const char *host) {
    tls_t *z = tls_of(tcp);

    /* Everything resets except the buffers, which are kept and reused:
       clearing those pointers would leak them and allocate again. */
    u8 *keep_rec = z->rec, *keep_hs = z->hs, *keep_app = z->app, *keep_chain = z->chain;
    memset(z, 0, sizeof(*z));
    z->rec = keep_rec; z->hs = keep_hs; z->app = keep_app; z->chain = keep_chain;
    z->tcp = tcp;

    if (!alloc_buffers(z)) { fail(z, "not enough memory for a connection"); return false; }

    u32 i = 0;
    while (host[i] && i < sizeof(z->host) - 1) { z->host[i] = host[i]; i++; }
    z->host[i] = 0;
    if (!i) { fail(z, "no host name to check a certificate against"); return false; }

    /* Without unpredictable bytes there is no point continuing: the key
       would be one anybody could work out, and the connection would look
       exactly as good as a real one. */
    if (!rng_ready()) {
        fail(z, "this machine has no source of randomness, so no key can be made");
        return false;
    }

    rng_bytes(z->priv, 32);
    rng_bytes(z->client_random, 32);
    rng_bytes(z->session_id, 32);
    x25519_public(z->priv, z->pub);

    sha256_init(&z->transcript);

    /* --- client hello --- */
    u8 *hello = z->rec;
    u32 hello_len = build_client_hello(z, hello + 4, REC_MAX - 4);
    if (!hello_len) { fail(z, "could not build the greeting"); return false; }
    hello[0] = HS_CLIENT_HELLO;
    hello[1] = (u8)(hello_len >> 16);
    hello[2] = (u8)(hello_len >> 8);
    hello[3] = (u8)hello_len;

    sha256_update(&z->transcript, hello, hello_len + 4);

    {
        /* Copied out because the record buffer is about to be reused. */
        u8 *copy = (u8 *)kmalloc(hello_len + 4);
        if (!copy) { fail(z, "not enough memory"); return false; }
        memcpy(copy, hello, hello_len + 4);
        bool sent = send_plain_record(z, REC_HANDSHAKE, copy, hello_len + 4);
        kfree(copy);
        if (!sent) { fail(z, "could not send the greeting"); return false; }
    }

    /* --- server hello --- */
    {
        u8 type; const u8 *body, *whole; u32 len, whole_len;
        if (!hs_next(z, false, &type, &body, &len, &whole, &whole_len)) return false;
        if (type != HS_SERVER_HELLO) { refuse(z, "the server did not answer with a hello", ALERT_UNEXPECTED_MESSAGE); return false; }
        if (!parse_server_hello(z, body, len)) return false;
        sha256_update(&z->transcript, whole, whole_len);
    }

    /* --- the shared secret, and the keys for the rest of the handshake --- */
    u8 shared[32];
    if (!x25519(z->priv, z->peer_share, shared)) {
        refuse(z, "the server's key share is not usable", ALERT_ILLEGAL_PARAMETER);
        return false;
    }

    {
        u8 zeros[32], early[32], derived[32], empty_hash[32], th[32];
        memset(zeros, 0, 32);
        hkdf_extract(0, 0, zeros, 32, early);
        sha256("", 0, empty_hash);
        if (!tls13_derive_secret(early, "derived", empty_hash, 32, derived)) {
            refuse(z, "the key schedule failed", ALERT_INTERNAL_ERROR); return false;
        }
        hkdf_extract(derived, 32, shared, 32, z->hs_secret);

        transcript_hash(z, th);
        if (!tls13_derive_secret(z->hs_secret, "c hs traffic", th, 32, z->c_hs) ||
            !tls13_derive_secret(z->hs_secret, "s hs traffic", th, 32, z->s_hs)) {
            refuse(z, "the key schedule failed", ALERT_INTERNAL_ERROR); return false;
        }
        if (!set_keys(z, z->s_hs, false)) { refuse(z, "could not set up decryption", ALERT_INTERNAL_ERROR); return false; }

        /* And ours, now rather than just before our finished: from here the
           server expects anything we say sealed, and what we may have to say
           before finished is why its flight was refused. Nothing else goes
           out before finished, so it still goes as record nought. */
        if (!set_keys(z, z->c_hs, true)) { refuse(z, "could not set up encryption", ALERT_INTERNAL_ERROR); return false; }
    }

    /* Everything from here is encrypted.
     *
     * The server's messages have one order and every one of them is required:
     * encrypted extensions, its certificate, its signature over the handshake,
     * then finished. Any other sequence is refused.
     *
     * This loop used to take the four in any order and stop at the first
     * finished, and nothing after it asked whether a signature had ever
     * arrived. Leaving the signature out was therefore a handshake that
     * passed, and that is the whole of server authentication gone. The
     * certificate chain is public -- anybody can fetch a site's -- and the
     * finished message is keyed from a secret the server agreed with us, so
     * whoever sits in the middle can compute it. The signature is the only
     * step that needs the site's private key. Someone on the path between
     * this machine and a bank could present the bank's real chain, skip the
     * signature, and read and rewrite the whole conversation, and the address
     * bar would have said it was the bank. */
    int step = TLS_FLIGHT_START;
    u8 hash_before_verify[32];

    u32 nchain = 0;
    u32 chain_used = 0;

    for (;;) {
        u8 type; const u8 *body, *whole; u32 len, whole_len;
        if (!hs_next(z, true, &type, &body, &len, &whole, &whole_len)) return false;

        /* Asked before anything in the message is looked at, so one that
           arrives out of turn is refused unread. */
        const char *why = 0;
        int next = tls_flight_step(step, type, &why);
        if (next < 0) { refuse(z, why, ALERT_UNEXPECTED_MESSAGE); return false; }
        step = next;

        if (type == HS_ENCRYPTED_EXT) {
            sha256_update(&z->transcript, whole, whole_len);

        } else if (type == HS_CERTIFICATE) {
            /* certificate_request_context, then a list of certificates each
               with its own extensions. */
            if (len < 1) { refuse(z, "the certificate message is malformed", ALERT_DECODE_ERROR); return false; }
            u32 p = 0;
            u32 ctx = body[p++];
            if (p + ctx + 3 > len) { refuse(z, "the certificate message is malformed", ALERT_DECODE_ERROR); return false; }
            p += ctx;
            u32 list = ((u32)body[p] << 16) | ((u32)body[p + 1] << 8) | body[p + 2];
            p += 3;
            if (p + list > len) { refuse(z, "the certificate list is malformed", ALERT_DECODE_ERROR); return false; }

            u32 end = p + list;
            while (p + 3 <= end && nchain < X509_MAX_CHAIN) {
                u32 clen = ((u32)body[p] << 16) | ((u32)body[p + 1] << 8) | body[p + 2];
                p += 3;
                if (p + clen > end) { refuse(z, "a certificate runs past the list", ALERT_DECODE_ERROR); return false; }
                if (chain_used + clen > HS_MAX) { refuse(z, "the certificates are too big", ALERT_INTERNAL_ERROR); return false; }
                memcpy(z->chain + chain_used, body + p, clen);
                z->ders[nchain] = z->chain + chain_used;
                z->lens[nchain] = clen;
                chain_used += clen;
                nchain++;
                p += clen;
                if (p + 2 > end) break;
                u32 ext = ((u32)body[p] << 8) | body[p + 1];
                p += 2 + ext;
            }
            if (nchain == 0) { refuse(z, "the server sent no certificate", ALERT_DECODE_ERROR); return false; }
            if (!x509_parse(z->ders[0], z->lens[0], &z->leaf)) {
                refuse(z, "the server's certificate could not be read", ALERT_BAD_CERTIFICATE);
                return false;
            }
            sha256_update(&z->transcript, whole, whole_len);

        } else if (type == HS_CERT_VERIFY) {
            /* Over everything up to and including the certificate, so the
               hash is taken before this message joins the transcript. */
            transcript_hash(z, hash_before_verify);
            if (!check_cert_verify(z, body, len, &z->leaf, hash_before_verify)) return false;
            sha256_update(&z->transcript, whole, whole_len);

        } else {
            /* The server's finished message is a MAC over everything before
               it, so the hash has to be taken before it joins the
               transcript. What comes after it, including the keys for the
               conversation proper, is over the transcript with it in. */
            u8 upto_here[32];
            transcript_hash(z, upto_here);

            u8 fkey[32], mac[32];
            if (!hkdf_expand_label(z->s_hs, "finished", 0, 0, fkey, 32)) {
                refuse(z, "the key schedule failed", ALERT_INTERNAL_ERROR); return false;
            }
            hmac_sha256(fkey, 32, upto_here, 32, mac);
            if (len != 32 || memcmp(mac, body, 32) != 0) {
                refuse(z, "the server's finished message is wrong", ALERT_DECRYPT_ERROR);
                return false;
            }
            sha256_update(&z->transcript, whole, whole_len);
            break;
        }
    }

    /* --- who it actually is ---------------------------------------------- */
    {
        u64 now = x509_now();
        x509_result_t r = x509_verify_chain(z->ders, z->lens, nchain, z->host, now);
        if (r != X509_OK) { refuse(z, x509_reason(r), alert_for_chain(r)); return false; }
    }

    /* --- our finished, and then the keys for the conversation proper ----- */
    {
        /* Everything through the server's finished message and nothing
           after it. Both the MAC below and the traffic secrets further down
           are over exactly this, and ours is not in it. */
        u8 th[32];
        transcript_hash(z, th);

        u8 fkey[32], mac[32], msg[36];
        if (!hkdf_expand_label(z->c_hs, "finished", 0, 0, fkey, 32)) {
            refuse(z, "the key schedule failed", ALERT_INTERNAL_ERROR); return false;
        }
        hmac_sha256(fkey, 32, th, 32, mac);
        msg[0] = HS_FINISHED;
        msg[1] = 0; msg[2] = 0; msg[3] = 32;
        memcpy(msg + 4, mac, 32);
        if (!send_encrypted(z, REC_HANDSHAKE, msg, 36)) {
            fail(z, "could not send the finished message");
            return false;
        }

        u8 zeros[32], derived[32], master[32], empty_hash[32];
        memset(zeros, 0, 32);
        sha256("", 0, empty_hash);
        if (!tls13_derive_secret(z->hs_secret, "derived", empty_hash, 32, derived)) {
            refuse(z, "the key schedule failed", ALERT_INTERNAL_ERROR); return false;
        }
        hkdf_extract(derived, 32, zeros, 32, master);

        /* The application keys are over the transcript through the server's
           finished message: that message included, ours not.
         *
           This was wrong once, by one message, and it is worth saying what
           that looked like from here. The handshake completed. The server
           accepted our finished, so it was satisfied. Then the first record
           it sent under the new keys would not authenticate, and the only
           thing this end could say about it was that a record did not
           authenticate, which is also what a forged record looks like. A key
           schedule that is wrong in a way both ends share is invisible; one
           that is wrong on one side only is invisible in a different way,
           and neither tells you which message you are out by. The check in
           selftest.c against RFC 8448 does. */
        if (!tls13_derive_secret(master, "c ap traffic", th, 32, z->c_app) ||
            !tls13_derive_secret(master, "s ap traffic", th, 32, z->s_app)) {
            refuse(z, "the key schedule failed", ALERT_INTERNAL_ERROR); return false;
        }
        if (!set_keys(z, z->c_app, true) || !set_keys(z, z->s_app, false)) {
            refuse(z, "could not set up the connection keys", ALERT_INTERNAL_ERROR);
            return false;
        }
        z->keys_are_app = true;
    }

    z->open = true;
    z->handshake_done = true;
    z->error[0] = 0;
    kformat(z->described, sizeof(z->described), "TLS 1.3, x25519, aes-128-gcm");
    memcpy(nowhere.described, z->described, sizeof(nowhere.described));
    bb_log("tls %s to %s", z->described, z->host);
    return true;
}

/* --- carrying data -------------------------------------------------------- */

bool tls_send(int tcp, const void *data, u32 len) {
    tls_t *z = tls_of(tcp);
    if (!z->open) return false;
    const u8 *p = (const u8 *)data;
    while (len) {
        u32 take = len > 8192 ? 8192 : len;
        if (!send_encrypted(z, REC_APPDATA, p, take)) return false;
        p += take;
        len -= take;
    }
    return true;
}

/* The server's move to new keys (RFC 8446 4.6.3). Its secret moves on one
   step, and so does its sequence number, back to nought; if it asks, ours
   follows -- after we have said so under the old key, which is the one it is
   still reading with.
 *
 * Before this, a KeyUpdate was skipped as if it were a session ticket, the
 * next record came under a key this end did not have, and the connection
 * ended as a record that did not authenticate. A long download from a
 * server that rotates its keys every so many records simply stopped, and
 * looked, to whoever was reading it, like a file that ended there. */
static bool key_update(tls_t *z, u8 requested) {
    u8 next[32];
    if (!hkdf_expand_label(z->s_app, "traffic upd", 0, 0, next, 32)) return false;
    memcpy(z->s_app, next, 32);
    if (!set_keys(z, z->s_app, false)) return false;
    if (requested) {
        u8 msg[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
        if (!send_encrypted(z, REC_HANDSHAKE, msg, 5)) return false;
        if (!hkdf_expand_label(z->c_app, "traffic upd", 0, 0, next, 32)) return false;
        memcpy(z->c_app, next, 32);
        if (!set_keys(z, z->c_app, true)) return false;
    }
    return true;
}

/* Handshake messages after the handshake: session tickets, which mean
   nothing because nothing resumes a connection yet, and key updates, which
   mean everything. Reassembled like the handshake's, since a message need
   not line up with the record it came in. */
static bool post_handshake(tls_t *z, const u8 *data, u32 len) {
    if (z->hs_len + len > HS_MAX) {
        refuse(z, "a message after the handshake is too big to hold", ALERT_INTERNAL_ERROR);
        return false;
    }
    memcpy(z->hs + z->hs_len, data, len);
    z->hs_len += len;

    for (;;) {
        u32 avail = z->hs_len - z->hs_pos;
        if (avail < 4) break;
        const u8 *p = z->hs + z->hs_pos;
        u32 n = ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
        if (n > HS_MAX) {
            refuse(z, "a message after the handshake claims an impossible size", ALERT_DECODE_ERROR);
            return false;
        }
        if (avail < 4 + n) break;
        if (p[0] == HS_KEY_UPDATE) {
            if (n != 1) { refuse(z, "a key update is malformed", ALERT_DECODE_ERROR); return false; }
            if (p[4] > 1) { refuse(z, "a key update asks for something that does not exist", ALERT_ILLEGAL_PARAMETER); return false; }
            if (!key_update(z, p[4])) {
                refuse(z, "a key update could not be carried out", ALERT_INTERNAL_ERROR);
                return false;
            }
        }
        z->hs_pos += 4 + n;
    }
    if (z->hs_pos) {
        memmove(z->hs, z->hs + z->hs_pos, z->hs_len - z->hs_pos);
        z->hs_len -= z->hs_pos;
        z->hs_pos = 0;
    }
    return true;
}

static u32 recv_on(tls_t *z, u8 *out, u32 cap, u32 timeout_ms);

u32 tls_recv(int tcp, u8 *out, u32 cap, u32 timeout_ms) {
    return recv_on(tls_of(tcp), out, cap, timeout_ms);
}

static u32 recv_on(tls_t *z, u8 *out, u32 cap, u32 timeout_ms) {
    if (!z->open) return 0;

    /* Anything already decrypted and not yet handed over. */
    if (z->app_pos < z->app_len) {
        u32 have = z->app_len - z->app_pos;
        u32 take = have < cap ? have : cap;
        memcpy(out, z->app + z->app_pos, take);
        z->app_pos += take;
        return take;
    }
    if (z->ended) return 0;

    u64 deadline = timer_ticks() + (u64)timeout_ms * timer_hz() / 1000;
    for (;;) {
        u64 now = timer_ticks();
        u32 left = now >= deadline ? 0 : (u32)((deadline - now) * 1000 / timer_hz());

        /* Not yet is not the end: a server quiet for a while is still
           answering, and the caller decides how long to wait for it. */
        u8 type; u32 len;
        int r = read_record(z, &type, &len, true, left);
        if (r == 0) return 0;
        if (r < 0) {
            z->ended = true;
            return 0;
        }
        if (z->ended) return 0;
        if (type == REC_CHANGE_CIPHER) continue;

        if (type == REC_HANDSHAKE) {
            if (!post_handshake(z, z->rec, len)) { z->ended = true; return 0; }
            continue;
        }
        if (type != REC_APPDATA) continue;
        if (len == 0) continue;

        memcpy(z->app, z->rec, len);
        z->app_len = len;
        z->app_pos = 0;

        u32 take = len < cap ? len : cap;
        memcpy(out, z->app, take);
        z->app_pos = take;
        return take;
    }
}

/* The session on a connection whose program was ended from outside. No close
   notification: sending one waits on the network, and this runs where nothing
   may wait (see syscall_abandon). */
void tls_abandon(int tcp) {
    tls_t *z = tls_of(tcp);
    z->open = false;
    z->handshake_done = false;
    z->ended = false;
    z->app_len = z->app_pos = 0;
    z->hs_len = z->hs_pos = 0;
    z->have = 0;
}

void tls_close(int tcp) {
    tls_t *z = tls_of(tcp);
    if (z->open) {
        /* A close notification, so the other end can tell a finished
           conversation from a cut cable. */
        u8 alert[2] = { 1, 0 };
        send_encrypted(z, REC_ALERT, alert, 2);
    }
    z->open = false;
    z->handshake_done = false;
    z->ended = false;
    z->app_len = z->app_pos = 0;
    z->hs_len = z->hs_pos = 0;
    z->have = 0;
}

/* --- for the self test -----------------------------------------------------
 *
 * A session of its own, never joined to a connection, with the network
 * swapped for buffers: what this layer writes, to the byte, and what it does
 * with bytes it is given, without a server. */

static tls_t scratch;

static bool scratch_begin(void) {
    memset(&scratch, 0, sizeof(scratch));
    scratch.tcp = -1;
    scratch.rec = (u8 *)kmalloc(REC_MAX + 64);
    scratch.hs  = (u8 *)kmalloc(HS_MAX);
    scratch.app = (u8 *)kmalloc(REC_MAX);
    return scratch.rec && scratch.hs && scratch.app;
}

static void scratch_end(void) {
    kfree(scratch.rec); kfree(scratch.hs); kfree(scratch.app);
    memset(&scratch, 0, sizeof(scratch));
    test_in = 0; test_out = 0;
    test_in_waits = false;
}

u32 tls_test_alert(const u8 *secret, u8 desc, u8 *out, u32 cap) {
    if (!scratch_begin()) { scratch_end(); return 0; }
    u32 n = 0;
    if (!secret || set_keys(&scratch, secret, true)) {
        test_out = out; test_out_len = 0; test_out_cap = cap; test_writes = 0;
        send_alert(&scratch, desc);
        n = test_out_len;
    }
    scratch_end();
    return n;
}

u32 tls_test_recv(const u8 s_app[32], const u8 c_app[32], const u8 *in, u32 in_len,
                  u8 *got, u32 got_cap, u8 *sent, u32 sent_cap, u32 *sent_len,
                  u8 s_after[32], u8 c_after[32]) {
    if (!scratch_begin()) { scratch_end(); return 0; }
    memcpy(scratch.s_app, s_app, 32);
    memcpy(scratch.c_app, c_app, 32);
    u32 n = 0;
    if (set_keys(&scratch, s_app, false) && set_keys(&scratch, c_app, true)) {
        scratch.open = scratch.handshake_done = scratch.keys_are_app = true;
        test_in = in; test_in_len = in_len; test_in_pos = 0;
        test_out = sent; test_out_len = 0; test_out_cap = sent_cap; test_writes = 0;
        n = recv_on(&scratch, got, got_cap, 1000);
        *sent_len = test_out_len;
        memcpy(s_after, scratch.s_app, 32);
        memcpy(c_after, scratch.c_app, 32);
    }
    scratch_end();
    return n;
}

u32 tls_test_recv_split(const u8 s_app[32], const u8 *in, u32 first, u32 in_len,
                        u8 *got, u32 got_cap, u32 *first_got, bool *ended_between) {
    if (!scratch_begin()) { scratch_end(); return 0; }
    memcpy(scratch.s_app, s_app, 32);
    u32 n = 0;
    *first_got = 0;
    *ended_between = true;
    if (set_keys(&scratch, s_app, false)) {
        scratch.open = scratch.handshake_done = scratch.keys_are_app = true;
        test_in = in; test_in_len = first; test_in_pos = 0; test_in_waits = true;
        *first_got = recv_on(&scratch, got, got_cap, 1000);
        *ended_between = scratch.ended;
        test_in_len = in_len;
        n = recv_on(&scratch, got, got_cap, 1000);
    }
    scratch_end();
    return n;
}
