"""A page's WebSocket, against a server that is not this project's.

The page (webserver.py, /ws-test) opens one socket after another -- an echo,
a server that closes first, a server whose answer to the upgrade is for the
wrong key, one that picks a subprotocol, one that sends binary -- and when it
has finished goes to /ws-done with everything it saw. That address is what
this reads, off the server, so a browser that never got there fails here
rather than passing by printing nothing wrong.

What the page saw is half of it: a client marking its own work. The other
half is what only the server knows -- that every frame the browser sent was
masked, that it answered a ping and a close, and that the upgrade carried the
page's cookie and its origin.

  python tools/wscheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from webserver import Server                                    # noqa: E402

DISK = os.path.join(ROOT, "wscheck.%d.img" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)    # the terminal's default background

# What the page writes, a line a step, in order (webserver.py, WS_PAGE).
WANT = [
    ("refused SyntaxError SyntaxError InvalidAccessError",
     "a bad address, a subprotocol named twice and a bad close code are refused"),
    ("made 0 0 true", "a new socket is connecting, at the address it was given"),
    ("early InvalidStateError", "and cannot send before it is open"),
    ("open 1 []", "it opens"),
    ("echo s:hello é|s:fragmented|s:café \U0001F600|b:1.2.250|b:70000x7",
     "text past ASCII, a message in pieces, binary and 70000 bytes come and go"),
    ("closing 2", "close() leaves it closing until the server answers"),
    ("close 1000 true done 3", "and then it is closed cleanly, with the code and reason sent"),
    ("server close 4001 true bye", "a close the server starts is answered and reported"),
    ("bad accept error 1006 false", "an answer to the upgrade for the wrong key fails it"),
    ("protocol chat.v2", "the subprotocol the server picked is the one reported"),
    ("proto close 1005", "a close with no code is 1005"),
    ("blob true 4 0.1.254.255", "binary comes as a Blob unless asked otherwise"),
    ("blob close 4000 thanks", "and a page's own close code reaches the server"),
    ("done", "every step ran"),
]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("a page's WebSocket")

    with Server() as srv:
        vm = Guest(DISK, memory=256, extra=srv.qemu_args())
        try:
            vm.wait_boot()
            vm.run("dhcp", timeout=25)
            vm.type("desktop\n")
            mon = vm.monitor()
            mon.move_to(20, 20)
            mon.wait_screen("ws-desk",
                            lambda w, h, px: count_in(px, w, (0, 0, 1024, 768), TERMINAL) > 50000,
                            timeout=60)
            time.sleep(1)
            vm.type("browser http://%s/ws-test\n" % srv.host)

            # Until the page says it has finished, or it plainly will not.
            said = None
            deadline = time.time() + 240
            while time.time() < deadline and said is None:
                for how, text in srv.received():
                    if how == "ws-done":
                        said = text
                time.sleep(0.5)
            c.add("the page ran every step and said what it saw", said is not None)
            got = said.split(";") if said else []
            for i, (line, name) in enumerate(WANT):
                ok = i < len(got) and got[i] == line
                c.add(name, ok)
                if not ok:
                    print("      wanted %a, got %a" % (line, got[i] if i < len(got) else None))

            log = srv.websockets()
            opens = [what for path, what in log if what.startswith("open")]
            c.add("each upgrade carried the page's cookie and origin and version 13",
                  len(opens) >= 5 and all("cookie=wsjar=kept" in o and "origin=http://" in o
                                          and "version=13" in o for o in opens))
            c.add("every frame the browser sent was masked",
                  log and not any(what == "unmasked" for _, what in log))
            c.add("a ping was answered with a pong carrying the same words",
                  ("/ws/echo", "pong are you there") in log)
            c.add("the close the server started was answered with its code",
                  ("/ws/close", "close 4001 ") in log)
            c.add("the server was sent 70000 bytes in one message",
                  ("/ws/echo", "binary 70000") in log)
        finally:
            vm.stop()
            try:
                os.remove(DISK)
            except OSError:
                pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
