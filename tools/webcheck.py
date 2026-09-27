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
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402
from webserver import Server, INSTANT_IP                 # noqa: E402

DISK = os.path.join(ROOT, "webcheck.%d.img" % os.getpid())
PCAP = os.path.join(ROOT, "webcheck.%d.pcap" % os.getpid())


def syn_mss(path):
    """The segment size the guest offered in its first SYN, read from the
    capture QEMU wrote of the card's traffic: 0 for a SYN that offered none,
    None if there was no SYN at all.

    The MSS itself makes no difference here -- QEMU's user networking takes
    1460 when none is offered -- which is exactly why it has to be read off
    the wire: the transfers cannot show it, and a real server that assumed
    536 would have sent a third of what it could in every segment."""
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    at = 24                                   # past the file's own header
    while at + 16 <= len(data):
        incl = struct.unpack_from("<I", data, at + 8)[0]
        frame = data[at + 16:at + 16 + incl]
        at += 16 + incl
        if len(frame) < 54 or frame[12:14] != b"\x08\x00":
            continue
        ip = frame[14:]
        if ip[9] != 6:
            continue
        tcp = ip[(ip[0] & 15) * 4:]
        if len(tcp) < 20 or not (tcp[13] & 0x02) or (tcp[13] & 0x10):
            continue                          # a SYN, and not the answer to one
        opts = tcp[20:(tcp[12] >> 4) * 4]
        i = 0
        while i < len(opts) and opts[i] != 0:
            if opts[i] == 1:
                i += 1
                continue
            if i + 1 >= len(opts) or opts[i + 1] < 2:
                break
            if opts[i] == 2 and opts[i + 1] == 4:
                return struct.unpack(">H", opts[i + 2:i + 4])[0]
            i += opts[i + 1]
        return 0
    return None


def resent(path, port):
    """How many data segments the server on this port sent more than once,
    read from the same capture.

    A burst is a whole window, 45 frames, and the card's receive ring held 31
    of them: the rest were lost every time, and the downloads still came back
    whole, only a second and a half late while the server's timer ran out.
    Nothing in the transfers shows that except how long they take, which is
    not a thing to assert on; the resends are. A host that stalls the guest
    for longer than that timer would show here as well."""
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    seen = set()
    again = 0
    at = 24
    while at + 16 <= len(data):
        incl = struct.unpack_from("<I", data, at + 8)[0]
        frame = data[at + 16:at + 16 + incl]
        at += 16 + incl
        if len(frame) < 54 or frame[12:14] != b"\x08\x00":
            continue
        ip = frame[14:]
        if ip[9] != 6:
            continue
        total = struct.unpack(">H", ip[2:4])[0]
        tcp = ip[(ip[0] & 15) * 4:total]
        if len(tcp) < 20 or struct.unpack(">H", tcp[0:2])[0] != port:
            continue
        if len(tcp) - (tcp[12] >> 4) * 4 <= 0:
            continue                          # carries nothing to resend
        key = (struct.unpack(">H", tcp[2:4])[0], struct.unpack(">I", tcp[4:8])[0])
        if key in seen:
            again += 1
        seen.add(key)
    return again


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
        # The card named, so QEMU can be asked to write down its traffic.
        wire = ["-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
                "-object", "filter-dump,id=d0,netdev=n0,file=%s" % PCAP.replace("\\", "/")]
        if os.path.exists(PCAP):
            os.remove(PCAP)
        vm = Guest(DISK, memory=192, extra=wire)
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
            # Three times, because the fault this is here for did not happen
            # every time: a FIN taken out of order ended the connection with
            # a hole in the middle of the body, and whether the last segment
            # overtook anything was luck. One fetch found it about half the
            # time, which is a check that reports a working stack as often as
            # a broken one.
            #
            # A failing check that only says no costs another run to find out
            # what it saw, and this one is eight minutes in, so it says.
            whole = True
            for _ in range(3):
                big = fetched(vm, srv.host, "/size/200000")
                if big is None or big[1] != 200000:
                    print("      asked for 200000 bytes and got %s" % (big,))
                    whole = False
            c.add("a body far bigger than the receive buffer arrives whole,"
                  " three times over", whole)

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

        mss = syn_mss(PCAP)
        c.add("a connection offers a full segment size when it opens", mss == 1460)
        if mss != 1460:
            print("      the SYN offered %r" % (mss,))
        again = resent(PCAP, srv.port)
        c.add("a whole window arrives without the server sending any of it twice",
              again == 0)
        if again != 0:
            print("      the server sent %r segments again" % (again,))
        if not keep:
            try:
                os.remove(PCAP)
            except OSError:
                pass

        # --- and again on the other card ----------------------------------
        #
        # The PCnet is what VMware hands a guest it does not recognise, so it
        # is the card most people who try this will actually get, and it is
        # not the one anything else here is run against.
        #
        # The large body is the one that matters. Everything small passes on
        # a driver that drops frames, because there are not enough of them to
        # drop; this ring is sized to hold a whole advertised window and the
        # check is whether it does.
        other = Guest(DISK, memory=192, extra=srv.qemu_args(model="pcnet"))
        try:
            other.wait_boot()
            other.run("dhcp", timeout=25)
            c.add("the same pages come back off the other card",
                  fetched(other, srv.host, "/size/1000") == (200, 1000))
            c.add("and a body bigger than the window does not lose frames",
                  fetched(other, srv.host, "/size/200000", timeout=200)
                  == (200, 200000))
        finally:
            other.stop()

        # --- and a card nothing here can drive ----------------------------
        #
        # Which is the case this all started from: a machine with a network
        # controller sitting on the bus, no driver for it, and a report of
        # "no card" that sends you looking at the cable. The ids are the
        # whole value of the message, because they are what says whether a
        # driver could be written.
        stranded = Guest(DISK, memory=192, extra=["-nic", "user,model=ne2k_pci"])
        try:
            stranded.wait_boot()
            said = stranded.fresh("net", timeout=20)
            c.add("a card with no driver is not reported as no card",
                  "no network card" not in said)
            c.add("and it is named by the ids that say whose it is",
                  "10ec:8029" in said)
        finally:
            stranded.stop()

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
