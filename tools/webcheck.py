"""The machine fetching pages off a web server that is not part of it.

The self test can prove the checksum routine adds up. It cannot prove a
sequence number is right, because being right means agreeing with whatever
is at the other end, and both ends of a test this project writes agree with
each other by construction.

So there is a real server here, Python's own, and every check below is about
what it sends back. The two bugs this was written after were both invisible
to anything smaller: a segment sent one sequence number past where the peer
was waiting, which only shows against a server fast enough to reply inside
the call that sends to it; and a read that handed back the same bytes again
instead of taking them out of the buffer, which only shows on a body too big
to arrive at once.

  python tools/webcheck.py [--keep]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402
from webserver import Server, INSTANT_IP                 # noqa: E402

DISK = os.path.join(ROOT, "webcheck.%d.img" % os.getpid())


def fetched(vm, host, path, timeout=90):
    """How many bytes of body came back, or None if the fetch failed."""
    out = vm.fresh("fetch %s %s" % (host, path), timeout=timeout)
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("status "):
            # "status 200, 140 bytes of headers, 1000 bytes of body"
            parts = line.replace(",", " ").split()
            try:
                at = parts.index("body")
                return int(parts[1]), int(parts[at - 3])
            except (ValueError, IndexError):
                return None
    return None


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the web")

    with Server() as srv:
        vm = Guest(DISK, memory=192, extra=srv.qemu_args())
        try:
            vm.wait_boot()
            vm.run("dhcp", timeout=25)

            got = fetched(vm, srv.host, "/size/1000")
            c.add("a page comes back from a real web server",
                  got is not None and got[0] == 200)
            c.add("and it is the length the server said it was",
                  got is not None and got[1] == 1000)

            # Again, because the first one working and the rest not is what
            # a connection that does not clean up after itself looks like,
            # and it is the state this stack was in.
            again = fetched(vm, srv.host, "/size/1000")
            c.add("and the connection after it works too",
                  again is not None and again[1] == 1000)

            # Past the receive buffer, so the body cannot have arrived in one
            # piece and the reads have to walk through it. A read that handed
            # back its buffer without emptying it returns the first part of
            # this over and over.
            big = fetched(vm, srv.host, "/size/200000")
            c.add("a body far bigger than the receive buffer arrives whole",
                  big is not None and big[1] == 200000)

            # The three ways a server can say where the body ends. Each one
            # is a different path through the client, and a client that only
            # knows one of them works on about half the web.
            c.add("a body measured by its content length",
                  fetched(vm, srv.host, "/size/40000") == (200, 40000))

            slow = fetched(vm, srv.host, "/slow")
            c.add("a body that arrives in dribs is waited out",
                  slow is not None and slow[1] == 2000)

            c.add("a page that is not there says so",
                  (fetched(vm, srv.host, "/nothing-here") or (0,))[0] == 404)
        finally:
            vm.stop()

        # --- and a server that answers instantly ---------------------------
        #
        # One fetch, because this path carries one connection. It is here for
        # the case nothing else can produce: the reply handled between the
        # instruction that sends a segment and the next one. A stack that
        # works out a sequence number after sending gets every fetch wrong
        # here and no fetch wrong anywhere else.
        fast = Guest(DISK, memory=192, extra=srv.instant_args())
        try:
            fast.wait_boot()
            fast.run("dhcp", timeout=25)
            got = fetched(fast, INSTANT_IP, "/size/1000")
            c.add("a server that answers inside the send is talked to "
                  "correctly", got is not None and got[1] == 1000)
        finally:
            fast.stop()
            try:
                os.remove(DISK)
            except OSError:
                pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
