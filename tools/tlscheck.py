"""https, against servers that are not part of this project.

Everything else about TLS can be checked against a fixed answer, and the
kernel's own tests do: the key schedule against the trace in RFC 8448, the
curve arithmetic against published vectors, a real certificate chain against
the root store. All of those prove the arithmetic is right.

None of them prove this machine can talk to the web, because every one of
them is this implementation agreeing with itself about bytes recorded years
ago. The only thing that settles that is a server on the internet, chosen by
somebody else, presenting a certificate issued this year by an authority
that has never heard of any of this, and answering.

So this is deliberately an outward test. It needs a working connection and
it will fail without one, which is the right way round: a machine that
cannot reach the web should not report that its https works.

Several sites rather than one, because a single site is a single certificate
authority, a single chain shape and a single server implementation, and
passing against it says nothing about the second. Two of the ones here were
added after they were found to be refused, each for a reason the first three
could never have shown.

Sites with deliberately bad certificates were left out while TLS 1.2 was
refused before any certificate was looked at: badssl.com speaks only 1.2,
so they would have passed for the wrong reason. Since 0.116.0 a 1.2
handshake gets as far as the certificate, and three of them are here, each
refused for its own reason. The wrong name, the expired date and the
untrusted root are also checked in kernel/selftest.c, against a real chain
with those faults introduced on purpose, which does not depend on somebody
else's server staying misconfigured.

  python tools/tlscheck.py [--keep]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT              # noqa: E402

DISK = os.path.join(ROOT, "tlscheck.%d.img" % os.getpid())

# Name, what the answer has to contain to count as that site answering
# rather than as something in the middle answering for it, and the version
# and group the key was agreed in.
SITES = [
    ("www.google.com", "<title>Google</title>", "1.3, x25519"),
    ("example.com", "Example Domain", "1.3, x25519"),
    ("wikipedia.org", None, "1.3, x25519"),    # answers with a redirect, which is fine

    # These two are here because each of them was once refused, and for a
    # different reason, and both reasons looked identical from the outside:
    # "a signature in the chain is wrong", which is also what a forgery
    # looks like.
    #
    # The BBC's chain is cross-signed with SHA-384, and only SHA-256 had its
    # PKCS#1 wrapper written out. archive.org sends its own root, which is
    # self-signed with SHA-1, and verifying that self-signature is both
    # impossible here and pointless: a root signs itself, which proves
    # nothing. The store's copy is the one to believe.
    ("www.bbc.co.uk", None, "1.3, x25519"),
    ("archive.org", None, "1.3, x25519"),

    # Microsoft's servers have no x25519: they answer a hello offering it
    # with a request to start again with P-256, which was refused, so Office,
    # Outlook and their sign-in could not be reached at all. (www.office.com
    # answers / with a redirect and no body; this one says where to go.)
    ("login.microsoftonline.com", None, "1.3, p-256"),

    # Servers with nothing newer than TLS 1.2, refused until 0.116.0: signing
    # with RSA-PSS over x25519, with ECDSA over x25519, with RSA's older
    # padding over P-256, and with RSA-PSS over P-256 (Microsoft's account
    # sign-in).
    ("imgur.com", None, "1.2, x25519"),
    ("genius.com", None, "1.2, x25519"),
    ("arstechnica.com", None, "1.2, p-256"),
    ("login.live.com", None, "1.2, p-256"),
]

# TLS 1.2 servers whose certificates are wrong on purpose (badssl.com), which
# have to be refused for that reason. The file's opening said why these were
# left out: they speak only 1.2, which was refused before any certificate
# was looked at. Now a 1.2 handshake gets as far as the certificate, and
# these are what show it is still looked at.
BAD = [
    ("expired.badssl.com", "the certificate has expired"),
    ("wrong.host.badssl.com", "the certificate is for another site"),
    ("self-signed.badssl.com", "no trusted authority signed this"),
]


def status_of(out):
    m = re.search(r"status (\d+), (\d+) bytes of headers, (\d+) bytes", out)
    return (int(m.group(1)), int(m.group(3))) if m else None


def resolved(vm, host, tries=4):
    """Waits for the name to turn into an address.

    DNS here goes through QEMU's user networking to whatever resolver the
    host uses, and the kernel gives it five seconds. When this runs inside
    the gate there are half a dozen other virtual machines on the same host
    asking the same resolver, and five seconds stops being generous.

    A name that will not resolve is not a fact about TLS, so it is worth
    separating: the alternative is a red line saying the handshake failed
    when no handshake was ever attempted, which sends the next person
    reading it into the wrong half of the code."""
    for _ in range(tries):
        out = vm.fresh("resolve %s" % host, timeout=40)
        if " is " in out:
            return True
    return False


def fetch(vm, target, timeout=180, tries=2):
    """One fetch, retried once if nothing came back at all.

    Retrying a check is usually how a flaky test gets hidden rather than
    fixed, so this is deliberately narrow: it repeats only when there is no
    status line, which means the request did not complete. A handshake that
    completed and gave a wrong answer is not retried, because that is the
    failure this is here to catch."""
    out = ""
    for _ in range(tries):
        out = vm.fresh("fetch https://%s /" % target, timeout=timeout)
        if status_of(out):
            return out
    return out


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("https")

    vm = Guest(DISK, memory=256, extra=["-nic", "user,model=rtl8139"])
    try:
        vm.wait_boot()
        vm.run("dhcp", timeout=30)

        for host, marker, group in SITES:
            if not resolved(vm, host):
                c.add("%s resolves, so there is something to connect to"
                      % host, False)
                print("      %s did not resolve after four tries; nothing "
                      "about TLS was tested against it" % host)
                continue

            out = fetch(vm, host)
            got = status_of(out)

            ok = "TLS %s, aes-128-gcm" % group in out
            c.add("%s completes a handshake" % host, ok)
            c.add("and answers", got is not None)
            if not ok or got is None:
                # What it actually said, because "the handshake failed" on
                # its own is the least useful sentence in this file.
                for line in out.splitlines()[1:6]:
                    print("      | %s" % line)
            if got is None:
                continue
            code, body = got
            c.add("with a status and a body", code >= 200 and body > 0)
            if marker:
                c.add("and the body is really %s's" % host, marker in out
                      or body > 400)

        for host, reason in BAD:
            if not resolved(vm, host):
                c.add("%s resolves, so there is something to connect to" % host, False)
                continue
            out = vm.fresh("fetch https://%s /" % host, timeout=120)
            c.add("%s is refused" % host, "secure:" not in out and status_of(out) is None)
            c.add("because %s" % reason, reason in out)
            if reason not in out:
                for line in out.splitlines()[1:6]:
                    print("      | %s" % line)

        # Something that is not a TLS server at all, on a port that answers.
        # The certificate checks are covered against a real chain by the
        # kernel's own tests, which can put a wrong name and an expired date
        # in front of the verifier deliberately; what cannot be arranged
        # there is a server that answers with something else entirely, and a
        # record layer that reads whatever arrives as though it were a
        # handshake is how that becomes interesting.
        if resolved(vm, "example.com"):
            out = vm.fresh("fetch https://example.com:80 /", timeout=90)
            c.add("a server that is not speaking TLS is refused",
                  "secure:" not in out and "status 2" not in out)
            c.add("and it says so rather than hanging", "tls:" in out)
            if "tls:" not in out:
                for line in out.splitlines()[1:6]:
                    print("      | %s" % line)

        # Two secure connections at once, from a program rather than the
        # kernel's own fetch, since the limit was in the system call: there
        # was one on the whole machine, and every program got NET_ERR_BUSY
        # while another -- the browser, sitting on an https page -- held it.
        if resolved(vm, "example.com") and resolved(vm, "www.google.com"):
            out = vm.fresh("exec /bin/wiretest two-secure example.com www.google.com",
                           timeout=180)
            c.add("a program opens a second secure connection while its first is open",
                  "PASS  and a second opens while the first is still open" in out)
            c.add("and both answer",
                  "PASS  the second answers" in out
                  and "PASS  and so does the first, which was waiting all that time" in out)
            if "WIRETEST_PASS" not in out:
                for line in out.splitlines()[1:8]:
                    print("      | %s" % line)

        # http still works, because breaking it to add https would be a
        # strange way to finish.
        plain = vm.fresh("fetch http://example.com /", timeout=90)
        c.add("and plain http still works", status_of(plain) is not None)
        c.add("and is not quietly encrypted",
              "TLS 1." not in plain)
    finally:
        vm.stop()
        if not keep and os.path.exists(DISK):
            os.remove(DISK)

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
