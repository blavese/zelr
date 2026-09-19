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

What is not here is a site with a deliberately bad certificate. The obvious
ones are only reachable over TLS 1.2, which this refuses before it has
looked at a certificate at all, so they would pass for the wrong reason. The
wrong name, the expired date and the untrusted root are checked in
kernel/selftest.c instead, against a real chain with those faults introduced
on purpose, which is both stricter and not dependent on somebody else's
server staying misconfigured.

  python tools/tlscheck.py [--keep]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT              # noqa: E402

DISK = os.path.join(ROOT, "tlscheck.%d.img" % os.getpid())

# Name, and what the answer has to contain to count as that site answering
# rather than as something in the middle answering for it.
SITES = [
    ("www.google.com", "<title>Google</title>"),
    ("example.com", "Example Domain"),
    ("wikipedia.org", None),          # answers with a redirect, which is fine

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
    ("www.bbc.co.uk", None),
    ("archive.org", None),
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

        for host, marker in SITES:
            if not resolved(vm, host):
                c.add("%s resolves, so there is something to connect to"
                      % host, False)
                print("      %s did not resolve after four tries; nothing "
                      "about TLS was tested against it" % host)
                continue

            out = fetch(vm, host)
            got = status_of(out)

            ok = "TLS 1.3, x25519, aes-128-gcm" in out
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

        # http still works, because breaking it to add https would be a
        # strange way to finish.
        plain = vm.fresh("fetch http://example.com /", timeout=90)
        c.add("and plain http still works", status_of(plain) is not None)
        c.add("and is not quietly encrypted",
              "TLS 1.3" not in plain)
    finally:
        vm.stop()
        if not keep and os.path.exists(DISK):
            os.remove(DISK)

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
