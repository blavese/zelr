/* X.509 certificates: reading them, and checking a chain of them.
 *
 * Everything here parses bytes chosen by a stranger. The rule followed
 * throughout is that a length in the data is a claim and not a fact: it is
 * checked against the end of the buffer before anything is read on the
 * strength of it, every time, including in the places where it obviously
 * cannot be wrong.
 *
 * The other rule is that the answer is one of a small set of reasons, and a
 * caller cannot accidentally treat a failure as a success. There is no
 * "mostly valid". */
#include "x509.h"
#include "sha256.h"
#include "sha512.h"
#include "rtc.h"
#include "string.h"
#include "roots.h"

/* --- DER ----------------------------------------------------------------- */

typedef struct {
    const u8 *p;
    const u8 *end;
} der_t;

static void der_init(der_t *d, const u8 *p, u32 len) {
    d->p = p;
    d->end = p + len;
}

static bool der_done(const der_t *d) { return d->p >= d->end; }

/* Reads one element. Returns its tag and the bounds of its content, and
   leaves the reader on the element after it. */
static bool der_read(der_t *d, u8 *tag, const u8 **content, u32 *len) {
    if (d->end - d->p < 2) return false;

    *tag = *d->p++;

    u32 n = *d->p++;
    if (n == 0x80) return false;              /* indefinite length is not DER */
    if (n & 0x80) {
        u32 count = n & 0x7f;
        if (count > 4) return false;          /* longer than anything real */
        if ((u32)(d->end - d->p) < count) return false;
        n = 0;
        for (u32 i = 0; i < count; i++) n = (n << 8) | *d->p++;
        /* A length written in more bytes than it needs is not DER, and is
           the sort of thing that is only ever done on purpose. */
        if (count == 1 && n < 0x80) return false;
    }

    if ((u32)(d->end - d->p) < n) return false;
    *content = d->p;
    *len = n;
    d->p += n;
    return true;
}

/* Reads an element that must have this tag. */
static bool der_expect(der_t *d, u8 want, const u8 **content, u32 *len) {
    u8 tag;
    if (!der_read(d, &tag, content, len)) return false;
    return tag == want;
}

/* Steps into a constructed element, giving a reader over its content. */
static bool der_enter(der_t *d, u8 want, der_t *inner) {
    const u8 *c; u32 l;
    if (!der_expect(d, want, &c, &l)) return false;
    der_init(inner, c, l);
    return true;
}

#define DER_INT   0x02
#define DER_BITS  0x03
#define DER_OCTET 0x04
#define DER_NULL  0x05
#define DER_OID   0x06
#define DER_UTF8  0x0c
#define DER_SEQ   0x30
#define DER_SET   0x31
#define DER_PRINT 0x13
#define DER_IA5   0x16
#define DER_UTC   0x17
#define DER_GEN   0x18
#define DER_BOOL  0x01

static bool oid_is(const u8 *p, u32 len, const u8 *want, u32 want_len) {
    return len == want_len && memcmp(p, want, len) == 0;
}

/* The object identifiers that matter, as the bytes they actually are. */
static const u8 OID_RSA[]        = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
static const u8 OID_RSA_SHA256[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
static const u8 OID_RSA_SHA384[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c};
static const u8 OID_RSA_SHA512[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0d};
static const u8 OID_RSA_PSS[]    = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0a};
static const u8 OID_EC_KEY[]     = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
static const u8 OID_P256[]       = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
static const u8 OID_P384[]       = {0x2b,0x81,0x04,0x00,0x22};
static const u8 OID_ECDSA_SHA256[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
static const u8 OID_ECDSA_SHA384[] = {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03};
static const u8 OID_BASIC_CONSTRAINTS[] = {0x55,0x1d,0x13};
static const u8 OID_SAN[]               = {0x55,0x1d,0x11};

static x509_sig_t sig_alg_from_oid(const u8 *p, u32 len) {
    if (oid_is(p, len, OID_RSA_SHA256, sizeof(OID_RSA_SHA256)))   return X509_SIG_RSA_SHA256;
    if (oid_is(p, len, OID_RSA_SHA384, sizeof(OID_RSA_SHA384)))   return X509_SIG_RSA_SHA384;
    if (oid_is(p, len, OID_RSA_SHA512, sizeof(OID_RSA_SHA512)))   return X509_SIG_RSA_SHA512;
    if (oid_is(p, len, OID_RSA_PSS,    sizeof(OID_RSA_PSS)))      return X509_SIG_RSA_PSS;
    if (oid_is(p, len, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256))) return X509_SIG_ECDSA_SHA256;
    if (oid_is(p, len, OID_ECDSA_SHA384, sizeof(OID_ECDSA_SHA384))) return X509_SIG_ECDSA_SHA384;
    return X509_SIG_NONE;
}

/* --- time ---------------------------------------------------------------- */

static bool digits(const u8 *p, u32 n, u32 *out) {
    u32 v = 0;
    for (u32 i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + (u32)(p[i] - '0');
    }
    *out = v;
    return true;
}

/* Both encodings into one packed number, so that comparing two times is
   comparing two integers and there is no calendar arithmetic anywhere. */
static bool parse_time(u8 tag, const u8 *p, u32 len, u64 *out) {
    u32 year, mon, day, hour, min, sec = 0;
    u32 i = 0;

    if (tag == DER_UTC) {
        if (len < 11) return false;
        u32 yy;
        if (!digits(p, 2, &yy)) return false;
        /* Two digit years, with the cutoff the specification gives. */
        year = yy < 50 ? 2000 + yy : 1900 + yy;
        i = 2;
    } else if (tag == DER_GEN) {
        if (len < 13) return false;
        if (!digits(p, 4, &year)) return false;
        i = 4;
    } else {
        return false;
    }

    if (len < i + 8) return false;
    if (!digits(p + i, 2, &mon))  return false;
    if (!digits(p + i + 2, 2, &day))  return false;
    if (!digits(p + i + 4, 2, &hour)) return false;
    if (!digits(p + i + 6, 2, &min))  return false;
    i += 8;
    if (len >= i + 2 && p[i] >= '0' && p[i] <= '9') {
        if (!digits(p + i, 2, &sec)) return false;
    }

    if (mon < 1 || mon > 12 || day < 1 || day > 31) return false;
    if (hour > 23 || min > 59 || sec > 60) return false;

    *out = (u64)year * 10000000000ULL + (u64)mon * 100000000ULL
         + (u64)day * 1000000ULL + (u64)hour * 10000ULL
         + (u64)min * 100ULL + sec;
    return true;
}

u64 x509_now(void) {
    rtc_time_t t;
    if (!rtc_read(&t)) return 0;
    /* The clock is read as if it were UTC. On a machine whose firmware
       keeps local time this is out by the offset, which matters only within
       a day of a certificate's edges, and saying so is better than adding a
       timezone database to find out. */
    return (u64)t.year * 10000000000ULL + (u64)t.month * 100000000ULL
         + (u64)t.day * 1000000ULL + (u64)t.hour * 10000ULL
         + (u64)t.minute * 100ULL + t.second;
}

/* --- the public key ------------------------------------------------------ */

static bool parse_spki(const u8 *p, u32 len, x509_t *out) {
    /* SubjectPublicKeyInfo ::= SEQUENCE {
           algorithm  AlgorithmIdentifier,       -- itself a SEQUENCE
           subjectPublicKey BIT STRING }
       Two levels, and reading the first child of the outer one as though it
       were the algorithm identifier fails on every certificate there is. */
    der_t outer, spki, alg;
    der_init(&outer, p, len);
    if (!der_enter(&outer, DER_SEQ, &spki)) return false;
    if (!der_enter(&spki, DER_SEQ, &alg)) return false;

    const u8 *oid; u32 oid_len;
    if (!der_expect(&alg, DER_OID, &oid, &oid_len)) return false;

    const u8 *bits; u32 bits_len;
    if (!der_expect(&spki, DER_BITS, &bits, &bits_len)) return false;
    if (bits_len < 2 || bits[0] != 0) return false;    /* no unused bits here */
    bits++; bits_len--;

    if (oid_is(oid, oid_len, OID_RSA, sizeof(OID_RSA))) {
        der_t key, seq;
        der_init(&key, bits, bits_len);
        if (!der_enter(&key, DER_SEQ, &seq)) return false;

        const u8 *n; u32 n_len;
        const u8 *e; u32 e_len;
        if (!der_expect(&seq, DER_INT, &n, &n_len)) return false;
        if (!der_expect(&seq, DER_INT, &e, &e_len)) return false;

        /* A leading zero is the encoding saying the number is positive, not
           part of the number. */
        while (n_len && n[0] == 0) { n++; n_len--; }
        if (n_len == 0 || n_len > RSA_MAX_BYTES) return false;
        /* Anything under a thousand bits has been broken in public. */
        if (n_len < 128) return false;

        while (e_len && e[0] == 0) { e++; e_len--; }
        if (e_len == 0 || e_len > 4) return false;
        u32 ev = 0;
        for (u32 i = 0; i < e_len; i++) ev = (ev << 8) | e[i];
        if (ev < 3 || !(ev & 1)) return false;

        out->key_type = X509_KEY_RSA;
        memcpy(out->rsa.n, n, n_len);
        out->rsa.n_len = n_len;
        out->rsa.e = ev;
        return true;
    }

    if (oid_is(oid, oid_len, OID_EC_KEY, sizeof(OID_EC_KEY))) {
        const u8 *curve; u32 curve_len;
        if (!der_expect(&alg, DER_OID, &curve, &curve_len)) return false;

        if (oid_is(curve, curve_len, OID_P256, sizeof(OID_P256)))
            out->curve = EC_P256;
        else if (oid_is(curve, curve_len, OID_P384, sizeof(OID_P384)))
            out->curve = EC_P384;
        else
            return false;

        u32 want = 1 + 2 * ec_field_bytes(out->curve);
        if (bits_len != want || bits[0] != 0x04) return false;

        out->key_type = X509_KEY_EC;
        memcpy(out->ec, bits, bits_len);
        out->ec_len = bits_len;
        return true;
    }

    return false;
}

/* --- extensions ---------------------------------------------------------- */

static bool parse_extensions(der_t *exts, x509_t *out) {
    while (!der_done(exts)) {
        der_t ext;
        if (!der_enter(exts, DER_SEQ, &ext)) return false;

        const u8 *oid; u32 oid_len;
        if (!der_expect(&ext, DER_OID, &oid, &oid_len)) return false;

        /* The critical flag is optional and defaults to false. */
        u8 tag; const u8 *c; u32 l;
        const u8 *save = ext.p;
        if (!der_read(&ext, &tag, &c, &l)) return false;
        if (tag == DER_BOOL) {
            if (!der_read(&ext, &tag, &c, &l)) return false;
        } else {
            ext.p = save;
            if (!der_read(&ext, &tag, &c, &l)) return false;
        }
        if (tag != DER_OCTET) return false;

        if (oid_is(oid, oid_len, OID_BASIC_CONSTRAINTS,
                   sizeof(OID_BASIC_CONSTRAINTS))) {
            der_t bc, inner;
            der_init(&bc, c, l);
            if (!der_enter(&bc, DER_SEQ, &inner)) return false;
            out->has_basic_constraints = true;
            out->is_ca = false;
            out->path_len = -1;
            if (!der_done(&inner)) {
                const u8 *v; u32 vl;
                const u8 *mark = inner.p;
                u8 t;
                if (!der_read(&inner, &t, &v, &vl)) return false;
                if (t == DER_BOOL) {
                    out->is_ca = (vl == 1 && v[0] != 0);
                } else {
                    inner.p = mark;
                }
            }
            if (!der_done(&inner)) {
                const u8 *v; u32 vl;
                if (der_expect(&inner, DER_INT, &v, &vl) && vl && vl <= 2) {
                    int n = 0;
                    for (u32 i = 0; i < vl; i++) n = (n << 8) | v[i];
                    out->path_len = n;
                }
            }
        } else if (oid_is(oid, oid_len, OID_SAN, sizeof(OID_SAN))) {
            out->san = c;
            out->san_len = l;
        }
    }
    return true;
}

/* --- a whole certificate -------------------------------------------------- */

bool x509_parse(const u8 *der, u32 len, x509_t *out) {
    memset(out, 0, sizeof(*out));
    out->path_len = -1;

    der_t top, cert;
    der_init(&top, der, len);
    if (!der_enter(&top, DER_SEQ, &cert)) return false;

    /* The signature covers the encoding of the first element including its
       header, so the bounds are taken before reading into it rather than
       reconstructed afterwards. */
    const u8 *tbs_start = cert.p;
    der_t tbs;
    if (!der_enter(&cert, DER_SEQ, &tbs)) return false;
    out->tbs = tbs_start;
    out->tbs_len = (u32)(cert.p - tbs_start);

    /* The algorithm, which appears here and again inside, and must agree. */
    der_t outer_alg;
    if (!der_enter(&cert, DER_SEQ, &outer_alg)) return false;
    const u8 *oid; u32 oid_len;
    if (!der_expect(&outer_alg, DER_OID, &oid, &oid_len)) return false;
    out->sig_alg = sig_alg_from_oid(oid, oid_len);

    const u8 *sig; u32 sig_len;
    if (!der_expect(&cert, DER_BITS, &sig, &sig_len)) return false;
    if (sig_len < 2 || sig[0] != 0) return false;
    out->sig = sig + 1;
    out->sig_len = sig_len - 1;

    /* --- inside the signed part --- */

    /* The version, if it is there at all. */
    u8 tag; const u8 *c; u32 l;
    const u8 *mark = tbs.p;
    if (!der_read(&tbs, &tag, &c, &l)) return false;
    if (tag != 0xa0) tbs.p = mark;             /* no version: this is a v1 */

    if (!der_expect(&tbs, DER_INT, &c, &l)) return false;   /* serial number */

    der_t inner_alg;
    if (!der_enter(&tbs, DER_SEQ, &inner_alg)) return false;
    const u8 *ioid; u32 ioid_len;
    if (!der_expect(&inner_alg, DER_OID, &ioid, &ioid_len)) return false;
    /* Two statements of the algorithm that disagree means one of them is
       being shown to somebody and the other used. */
    if (!oid_is(ioid, ioid_len, oid, oid_len)) return false;

    const u8 *issuer_start = tbs.p;
    if (!der_expect(&tbs, DER_SEQ, &c, &l)) return false;
    out->issuer = issuer_start;
    out->issuer_len = (u32)(tbs.p - issuer_start);

    der_t validity;
    if (!der_enter(&tbs, DER_SEQ, &validity)) return false;
    {
        u8 t1, t2; const u8 *v1, *v2; u32 l1, l2;
        if (!der_read(&validity, &t1, &v1, &l1)) return false;
        if (!der_read(&validity, &t2, &v2, &l2)) return false;
        if (!parse_time(t1, v1, l1, &out->not_before)) return false;
        if (!parse_time(t2, v2, l2, &out->not_after)) return false;
    }

    const u8 *subject_start = tbs.p;
    if (!der_expect(&tbs, DER_SEQ, &c, &l)) return false;
    out->subject = subject_start;
    out->subject_len = (u32)(tbs.p - subject_start);

    const u8 *spki_start = tbs.p;
    if (!der_expect(&tbs, DER_SEQ, &c, &l)) return false;
    if (!parse_spki(spki_start, (u32)(tbs.p - spki_start), out)) return false;

    /* The optional trailer: two deprecated identifiers and the extensions. */
    while (!der_done(&tbs)) {
        if (!der_read(&tbs, &tag, &c, &l)) return false;
        if (tag == 0xa3) {
            der_t wrapper, exts;
            der_init(&wrapper, c, l);
            if (!der_enter(&wrapper, DER_SEQ, &exts)) return false;
            if (!parse_extensions(&exts, out)) return false;
        }
    }

    return true;
}

/* --- checking a signature ------------------------------------------------- */

static bool hash_for(x509_sig_t alg, const u8 *data, u32 len,
                     u8 *out, u32 *out_len) {
    switch (alg) {
        case X509_SIG_RSA_SHA256:
        case X509_SIG_RSA_PSS:
        case X509_SIG_ECDSA_SHA256:
            sha256(data, len, out); *out_len = 32; return true;
        case X509_SIG_RSA_SHA384:
        case X509_SIG_ECDSA_SHA384:
            sha384(data, len, out); *out_len = 48; return true;
        case X509_SIG_RSA_SHA512:
            sha512(data, len, out); *out_len = 64; return true;
        default:
            return false;
    }
}

/* ECDSA-Sig-Value ::= SEQUENCE { r INTEGER, s INTEGER }.
   Shared with the handshake, which has to read the same shape out of the
   signature a server makes over the transcript. */
bool x509_ecdsa_split(const u8 *sig, u32 len,
                      const u8 **r, u32 *r_len,
                      const u8 **s, u32 *s_len) {
    der_t d, seq;
    der_init(&d, sig, len);
    if (!der_enter(&d, DER_SEQ, &seq)) return false;
    if (!der_expect(&seq, DER_INT, r, r_len)) return false;
    if (!der_expect(&seq, DER_INT, s, s_len)) return false;
    if (!der_done(&seq)) return false;
    while (*r_len && (*r)[0] == 0) { (*r)++; (*r_len)--; }
    while (*s_len && (*s)[0] == 0) { (*s)++; (*s_len)--; }
    return *r_len && *s_len;
}

bool x509_signed_by(const x509_t *child, const x509_t *issuer) {
    u8 h[64]; u32 h_len = 0;
    if (!hash_for(child->sig_alg, child->tbs, child->tbs_len, h, &h_len))
        return false;

    switch (child->sig_alg) {
        case X509_SIG_RSA_SHA256:
        case X509_SIG_RSA_SHA384:
        case X509_SIG_RSA_SHA512:
            if (issuer->key_type != X509_KEY_RSA) return false;
            /* All three wrappers are written out now, and which one is used
               follows from the length of the digest rather than from
               anything a certificate says about itself. The note that used
               to stand here said nothing that mattered used SHA-384; the
               cross-signature over GlobalSign's R46 root does, and it is
               most of the BBC. */
            return rsa_verify_pkcs1(&issuer->rsa, child->sig, child->sig_len,
                                    h, h_len);

        case X509_SIG_RSA_PSS:
            if (issuer->key_type != X509_KEY_RSA) return false;
            return rsa_verify_pss(&issuer->rsa, child->sig, child->sig_len,
                                  h, h_len);

        case X509_SIG_ECDSA_SHA256:
        case X509_SIG_ECDSA_SHA384: {
            if (issuer->key_type != X509_KEY_EC) return false;
            const u8 *r, *s; u32 r_len, s_len;
            if (!x509_ecdsa_split(child->sig, child->sig_len, &r, &r_len, &s, &s_len))
                return false;
            return ec_verify(issuer->curve, issuer->ec, issuer->ec_len,
                             h, h_len, r, r_len, s, s_len);
        }

        default:
            return false;
    }
}

/* --- names ---------------------------------------------------------------- */

static u8 lower(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c; }

static bool same_fold(const u8 *a, u32 a_len, const char *b) {
    u32 i = 0;
    for (; i < a_len; i++) {
        if (!b[i]) return false;
        if (lower(a[i]) != lower((u8)b[i])) return false;
    }
    return b[i] == 0;
}

/* One name out of a certificate against the host that was asked for.
 *
 * A wildcard covers exactly one label and only the leftmost one, so
 * *.example.com is a.example.com and is not example.com and is not
 * a.b.example.com. Implementations that are looser than this have accepted
 * certificates for the wrong site. */
static bool one_name_matches(const u8 *name, u32 name_len, const char *host) {
    if (name_len == 0) return false;

    if (name_len >= 2 && name[0] == '*' && name[1] == '.') {
        /* The host must have a dot, and everything after its first one must
           equal everything after the wildcard's. */
        const char *dot = host;
        while (*dot && *dot != '.') dot++;
        if (*dot != '.') return false;
        if (dot == host) return false;               /* nothing before it */
        /* And the part being covered must not itself contain a dot, which
           is the same condition read from the other side. */
        return same_fold(name + 2, name_len - 2, dot + 1);
    }

    return same_fold(name, name_len, host);
}

bool x509_host_matches(const x509_t *cert, const char *host) {
    if (!host || !host[0]) return false;

    /* Only the subject alternative name is consulted. The common name was
       used for this once and has not been the right answer for many years:
       a certificate without a subject alternative name does not identify a
       host, whatever its subject says. */
    if (!cert->san || !cert->san_len) return false;

    der_t d, names;
    der_init(&d, cert->san, cert->san_len);
    if (!der_enter(&d, DER_SEQ, &names)) return false;

    while (!der_done(&names)) {
        u8 tag; const u8 *c; u32 l;
        if (!der_read(&names, &tag, &c, &l)) return false;
        if (tag == 0x82) {                          /* dNSName */
            if (one_name_matches(c, l, host)) return true;
        }
    }
    return false;
}

/* --- the chain ------------------------------------------------------------ */

const char *x509_reason(x509_result_t r) {
    switch (r) {
        case X509_OK:             return "ok";
        case X509_BAD_PARSE:      return "the certificate could not be read";
        case X509_BAD_SIGNATURE:  return "a signature in the chain is wrong";
        case X509_NOT_YET_VALID:  return "the certificate is not valid yet";
        case X509_EXPIRED:        return "the certificate has expired";
        case X509_UNTRUSTED:      return "no trusted authority signed this";
        case X509_WRONG_NAME:     return "the certificate is for another site";
        case X509_NOT_A_CA:       return "a certificate in the chain may not sign others";
        case X509_UNSUPPORTED:    return "the certificate uses something unsupported";
        default:                  return "the certificate was refused";
    }
}

static bool dates_ok(const x509_t *c, u64 now, x509_result_t *why) {
    if (!now) return true;             /* no clock: dates cannot be judged */
    if (now < c->not_before) { *why = X509_NOT_YET_VALID; return false; }
    if (now > c->not_after)  { *why = X509_EXPIRED; return false; }
    return true;
}

x509_result_t x509_verify_chain(const u8 *const *ders, const u32 *lens, u32 n,
                                const char *host, u64 now) {
    if (n == 0 || n > X509_MAX_CHAIN) return X509_BAD_PARSE;

    static x509_t chain[X509_MAX_CHAIN];
    for (u32 i = 0; i < n; i++)
        if (!x509_parse(ders[i], lens[i], &chain[i])) return X509_BAD_PARSE;

    /* The leaf is for the host that was asked for, and not for whatever
       host the connection happened to reach. */
    if (!x509_host_matches(&chain[0], host)) return X509_WRONG_NAME;

    x509_result_t why = X509_BAD_SIGNATURE;
    if (!dates_ok(&chain[0], now, &why)) return why;

    /* Each certificate in turn, signed by the next one along, which must
       also be allowed to sign certificates at all. A chain where an
       ordinary leaf signs another certificate is the oldest hole there is. */
    u32 i = 0;
    for (; i + 1 < n; i++) {
        const x509_t *child = &chain[i];
        const x509_t *parent = &chain[i + 1];

        if (child->issuer_len != parent->subject_len ||
            memcmp(child->issuer, parent->subject, child->issuer_len) != 0)
            break;                                 /* not actually the parent */

        if (!parent->has_basic_constraints || !parent->is_ca)
            return X509_NOT_A_CA;
        if (parent->path_len >= 0 && (int)i > parent->path_len)
            return X509_NOT_A_CA;
        if (!dates_ok(parent, now, &why)) return why;
        if (!x509_signed_by(child, parent)) return X509_BAD_SIGNATURE;
    }

    /* And the top of what the server sent has to be signed by something
       this machine was told to trust. The server's own last certificate is
       not evidence of anything: it is checked against the trust store by
       its issuer's name and its signature, never accepted for being there. */
    const x509_t *top = &chain[i];

    x509_t anchor;
    const u8 *root; u32 root_len;

    /* A server is allowed to send the root itself, and many do. That
       certificate is still not evidence of anything by being there, but if
       the store holds one with the same subject then the store's own copy
       is the thing to believe, and the served one can be set aside
       entirely: the link below it is re-checked against the key this
       machine shipped with rather than against the key the server supplied.
       A forged copy of a root therefore fails here even though the loop
       above accepted the chain that led to it.
     *
       Doing it this way also sidesteps a question that has no good answer.
       A root is self-signed, and old ones are self-signed with SHA-1, which
       is not an algorithm to start accepting in order to read a web page.
       Go Daddy's Class 2 root is one of those and archive.org still serves
       it. Verifying a self-signature proves nothing anyway: it says the
       certificate was made by whoever made it. */
    if (roots_find(top->subject, top->subject_len, &root, &root_len)) {
        /* One certificate that is itself an anchor says nothing about a
           host, whatever name is on it. */
        if (i == 0) return X509_UNTRUSTED;
        if (!x509_parse(root, root_len, &anchor)) return X509_BAD_PARSE;
        if (!anchor.has_basic_constraints || !anchor.is_ca) return X509_NOT_A_CA;
        if (!x509_signed_by(&chain[i - 1], &anchor)) return X509_BAD_SIGNATURE;
        return X509_OK;
    }

    if (!roots_find(top->issuer, top->issuer_len, &root, &root_len))
        return X509_UNTRUSTED;
    if (!x509_parse(root, root_len, &anchor)) return X509_BAD_PARSE;
    if (!anchor.has_basic_constraints || !anchor.is_ca) return X509_NOT_A_CA;
    if (!x509_signed_by(top, &anchor)) return X509_BAD_SIGNATURE;

    return X509_OK;
}
