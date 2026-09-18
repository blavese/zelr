#pragma once
#include "types.h"
#include "rsa.h"
#include "ec.h"

/* Certificates, and what makes a chain of them mean something.
 *
 * This parses bytes chosen by whoever is at the other end of the
 * connection, before anything has been established about who they are. So
 * every length is checked against the buffer it came from and nothing is
 * read on the strength of a field saying it is there. A parser that trusts
 * its input is the wrong shape for this job regardless of how careful the
 * rest is.
 *
 * What a chain proves is narrow and worth stating: that somebody holding a
 * key this machine was configured to trust signed a statement that a
 * particular name goes with a particular key. It does not say the site is
 * honest or the content is safe. It says the bytes came from whoever holds
 * that name. */

#define X509_MAX_CHAIN 8

typedef enum { X509_KEY_NONE, X509_KEY_RSA, X509_KEY_EC } x509_keytype_t;

typedef enum {
    X509_SIG_NONE,
    X509_SIG_RSA_SHA256,
    X509_SIG_RSA_SHA384,
    X509_SIG_RSA_SHA512,
    X509_SIG_RSA_PSS,
    X509_SIG_ECDSA_SHA256,
    X509_SIG_ECDSA_SHA384
} x509_sig_t;

typedef struct {
    const u8 *tbs; u32 tbs_len;          /* exactly what the signature covers */
    x509_sig_t sig_alg;
    const u8 *sig; u32 sig_len;

    /* Names are kept as their raw encoding and compared byte for byte.
       Comparing them field by field means deciding what two differently
       spelled but equal names are, and the answer has been a source of
       forged chains. Byte equality is stricter than the standard allows
       and has never let the wrong certificate through. */
    const u8 *issuer;  u32 issuer_len;
    const u8 *subject; u32 subject_len;

    /* Packed as yyyymmddhhmmss, so comparison is integer comparison and
       there is no calendar arithmetic to get wrong. */
    u64 not_before, not_after;

    x509_keytype_t key_type;
    rsa_key_t rsa;
    ec_curve_t curve;
    u8  ec[97];
    u32 ec_len;

    bool is_ca;
    bool has_basic_constraints;
    int  path_len;                       /* -1 when unset */

    const u8 *san; u32 san_len;          /* the raw extension value */
} x509_t;

typedef enum {
    X509_OK,
    X509_BAD_PARSE,
    X509_BAD_SIGNATURE,
    X509_NOT_YET_VALID,
    X509_EXPIRED,
    X509_UNTRUSTED,
    X509_WRONG_NAME,
    X509_NOT_A_CA,
    X509_UNSUPPORTED
} x509_result_t;

const char *x509_reason(x509_result_t r);

bool x509_parse(const u8 *der, u32 len, x509_t *out);

/* Whether issuer's key made child's signature, over child's own bytes. */
bool x509_signed_by(const x509_t *child, const x509_t *issuer);

/* Whether this certificate was issued for this host, by the names in it
   rather than by whatever it was fetched from. */
bool x509_host_matches(const x509_t *cert, const char *host);

/* The time now, packed the same way as the fields above, or zero when the
   machine has no usable clock. */
u64 x509_now(void);

/* The whole thing: parse the chain the server sent, check each link, find a
   trusted key at the top, check the dates and check the name. Anything
   other than X509_OK means the connection must not be used. */
x509_result_t x509_verify_chain(const u8 *const *ders, const u32 *lens, u32 n,
                                const char *host, u64 now);
