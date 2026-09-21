"""Builds kernel/roots.c from a published list of certificate authorities.

kernel/roots.c has said "regenerate with tools/mkroots.py" for a long time
and this file did not exist, so the store could not be regenerated at all.
It held forty-eight authorities, which is enough for some of the web and not
for the rest of it: following a search result to an ordinary site failed
with "no trusted authority signed this", because the site was signed by one
of the ones that were not there.

What a root store is, and why it is generated rather than written: every
other part of https here is arithmetic with a right answer, and a mistake in
it shows up as a failure. This is a list of who to believe, and the only
thing that makes it correct is that it agrees with the list everybody else
is using. So it comes from a published store. What goes in is public
certificates -- a name, a public key, and a signature over the pair -- and
nothing in them is ever executed.

The offset of the subject name is stored beside each one so that finding an
issuer does not mean parsing every certificate in the store. Working that
out means walking the DER as far as the subject:

    Certificate ::= SEQUENCE {
        tbsCertificate ::= SEQUENCE {
            version        [0] EXPLICIT, optional
            serialNumber   INTEGER
            signature      AlgorithmIdentifier
            issuer         Name
            validity       Validity
            subject        Name            <- this one, header and all
            ...

  python tools/mkroots.py [BUNDLE.pem] [--check]

With --check it walks the store that is already there and reports whether
the offsets it would have written match the ones recorded, which is how the
walker below was shown to be right before it was used to replace anything.
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "kernel", "roots.c")


# --- just enough DER to find one field ------------------------------------

def der_header(b, at):
    """(tag, length, where the content starts) for the element at `at`."""
    tag = b[at]
    i = at + 1
    n = b[i]
    i += 1
    if n & 0x80:
        count = n & 0x7F
        if count == 0 or count > 4:
            raise ValueError("a length this file will not parse")
        n = 0
        for _ in range(count):
            n = (n << 8) | b[i]
            i += 1
    return tag, n, i


def subject_span(der):
    """Where the subject Name starts in a certificate, and how long it is,
    counting its own tag and length bytes -- because that whole encoding is
    what an issuer field is compared against."""
    _, _, cert_at = der_header(der, 0)              # Certificate
    _, tbs_len, tbs_at = der_header(der, cert_at)   # tbsCertificate

    at = tbs_at
    tag, n, content = der_header(der, at)
    if tag == 0xA0:                                 # version, when present
        at = content + n
        tag, n, content = der_header(der, at)

    at = content + n                                # past serialNumber
    _, n, content = der_header(der, at)
    at = content + n                                # past signature
    _, n, content = der_header(der, at)
    at = content + n                                # past issuer
    _, n, content = der_header(der, at)
    at = content + n                                # past validity

    tag, n, content = der_header(der, at)
    if tag != 0x30:
        raise ValueError("subject is not a sequence")
    end = content + n
    if end > tbs_at + tbs_len:
        raise ValueError("subject runs past the certificate")
    return at, end - at


def name_of(der, off, length):
    """The common name in a Name, for the comment above each certificate.
    Found by looking for the commonName object identifier and taking the
    string after it, which is enough for a label and not a parser."""
    want = bytes([0x06, 0x03, 0x55, 0x04, 0x03])
    body = der[off:off + length]
    i = body.find(want)
    if i < 0:
        return "?"
    j = i + len(want)
    if j + 2 > len(body):
        return "?"
    n = body[j + 1]
    text = body[j + 2:j + 2 + n]
    out = "".join(chr(c) if 32 <= c < 127 else "." for c in text)
    return out.replace("*/", "").strip() or "?"


# --- reading and writing ---------------------------------------------------

def pem_certs(text):
    out = []
    import base64
    for block in re.findall(
            r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----",
            text, re.S):
        try:
            out.append(base64.b64decode("".join(block.split())))
        except Exception:
            pass
    return out


def existing():
    """The certificates already in kernel/roots.c, and the offsets recorded
    for them, so the walker above can be checked against them."""
    s = io.open(OUT, encoding="utf-8", errors="replace").read()
    blobs = {}
    for m in re.finditer(r"static const u8 root_(\d+)\[\] = \{(.*?)\};", s, re.S):
        n = int(m.group(1))
        blobs[n] = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2)))
    table = {}
    for m in re.finditer(r"\{ root_(\d+), sizeof\(root_\d+\), (\d+), (\d+) \}", s):
        table[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
    return blobs, table


def check():
    blobs, table = existing()
    if not blobs:
        print("nothing to check: no certificates in", OUT)
        return 1
    bad = 0
    for n in sorted(blobs):
        want = table.get(n)
        try:
            got = subject_span(blobs[n])
        except Exception as e:
            print("  root_%-3d could not be walked: %s" % (n, e))
            bad += 1
            continue
        if want != got:
            print("  root_%-3d recorded %r, walked %r" % (n, want, got))
            bad += 1
    print("%d of %d agree" % (len(blobs) - bad, len(blobs)))
    return 1 if bad else 0


def emit(certs, path):
    seen = set()
    keep = []
    for der in certs:
        try:
            off, length = subject_span(der)
        except Exception:
            continue                       # not one this can index; leave it out
        key = der[off:off + length]
        if key in seen:
            continue                       # two copies of one authority
        seen.add(key)
        keep.append((der, off, length))

    w = []
    w.append("""/* The keys this machine is willing to believe.
 *
 * Generated, not written. These are the trust anchors: certificate
 * authorities whose signatures are accepted as the end of a chain. They are
 * the one part of https that cannot be worked out from first principles,
 * because their whole purpose is to agree with the rest of the world about
 * who is who. A root store invented here would be correct arithmetic that
 * trusted nobody.
 *
 * They are public certificates rather than code: no program here runs
 * anything from them, and each is only a name and a public key with a
 * signature over the pair. Regenerate with tools/mkroots.py.
 *
 * The subject offset is stored with each one so that looking up an issuer
 * does not mean parsing all of them. */
#include "roots.h"
""")
    for i, (der, off, length) in enumerate(keep):
        w.append("\n/* %s */\n" % name_of(der, off, length))
        w.append("static const u8 root_%d[] = {" % i)
        for k, b in enumerate(der):
            if k % 12 == 0:
                w.append("\n    ")
            w.append("0x%02x, " % b)
        w.append("\n};\n")

    w.append("\nstatic const root_t ROOTS[] = {\n")
    for i, (der, off, length) in enumerate(keep):
        w.append("\n    { root_%d, sizeof(root_%d), %d, %d },\n" % (i, i, off, length))
    w.append("};\n")

    w.append("""
u32 roots_count(void) { return (u32)(sizeof(ROOTS) / sizeof(ROOTS[0])); }

const root_t *roots_at(u32 i) {
    return i < roots_count() ? &ROOTS[i] : 0;
}

bool roots_find(const u8 *subject, u32 len, const u8 **out, u32 *out_len) {
    if (!subject || !len) return false;
    for (u32 i = 0; i < roots_count(); i++) {
        const root_t *r = &ROOTS[i];
        if (r->sub_len != len) continue;
        if (memcmp(r->der + r->sub_off, subject, len) != 0) continue;
        if (out) *out = r->der;
        if (out_len) *out_len = r->len;
        return true;
    }
    return false;
}
""")
    io.open(path, "w", encoding="utf-8", newline="\n").write("".join(w))
    return len(keep)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--check" in sys.argv:
        return check()

    if args:
        text = io.open(args[0], encoding="utf-8", errors="replace").read()
    else:
        try:
            import certifi
        except ImportError:
            print("give a PEM bundle, or install certifi", file=sys.stderr)
            return 2
        text = io.open(certifi.where(), encoding="utf-8", errors="replace").read()

    certs = pem_certs(text)
    if not certs:
        print("no certificates in that bundle", file=sys.stderr)
        return 2
    n = emit(certs, OUT)
    print("wrote %s with %d authorities (from %d in the bundle)"
          % (os.path.relpath(OUT, ROOT), n, len(certs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
