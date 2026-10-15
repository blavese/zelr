"""The roots the kernel trusts (kernel/roots.c), as a TLS context for the host's proxies.

The host build's TLS is Python's, and Python on Windows trusts what the machine's store holds, which
is not what the kernel holds: the store fetches roots as they are first needed, so a site whose root
the store had not yet seen failed on the host and not in the kernel (CNN's, under GlobalSign Root
E46), and the other way round a root the store has and the kernel has not would pass here and fail
there. Reading the kernel's own list makes the host trust exactly what the machine does. Like the
kernel, it does not go and fetch an intermediate a server left out.
"""
import os
import re
import ssl

ROOTS_C = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "kernel", "roots.c")


def kernel_roots_der(path=ROOTS_C):
    """Every root in kernel/roots.c, DER, one after another."""
    text = open(path, encoding="utf-8").read()
    out = b""
    for body in re.findall(r"static const u8 root_\d+\[\] = \{(.*?)\};", text, re.S):
        out += bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", body))
    return out


def context():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.load_verify_locations(cadata=kernel_roots_der())
    return ctx


if __name__ == "__main__":
    ctx = context()
    print("%d roots from %s" % (len(ctx.get_ca_certs()), os.path.normpath(ROOTS_C)))
