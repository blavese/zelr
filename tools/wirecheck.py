"""What goes over the wire, and how much of it, and how often.

Three things that a browser does between asking for a page and having one,
none of which can be seen by looking at the page afterwards:

  it asks for the body compressed and puts it back together, which is three
  to five times fewer bytes over a stack that carries one connection at a
  time

  it keeps the connection, so that a page and its pictures are one
  handshake rather than a dozen

  it remembers what a server asked it to remember, so a session survives a
  click

The first and the last are checked from inside the machine, by a program
that fetches over the same code the browser does. The middle one cannot be:
a client that opened a connection per request looks exactly the same from
inside itself as one that did not. So the server counts connections against
requests, and the check is about the server's count.

  python tools/wirecheck.py [--keep]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT          # noqa: E402
from webserver import Server                                 # noqa: E402

DISK = os.path.join(ROOT, "wirecheck.%d.img" % os.getpid())


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the wire")

    with Server() as srv:
        vm = Guest(DISK, memory=256, extra=srv.qemu_args())
        try:
            vm.wait_boot()
            vm.run("dhcp", timeout=25)
            srv.reset_counts()

            out = vm.run("exec /bin/wiretest http://%s" % srv.host,
                         timeout=180)

            c.add("the browser's own fetching runs against a real server",
                  "WIRETEST_PASS" in out or "WIRETEST_FAIL" in out)
            for line in out.split("\n"):
                if "PASS  " in line or "FAIL  " in line:
                    name = line.split("  ", 2)[-1].strip()
                    if name:
                        c.add(name, "PASS" in line)

            # --- and the part only the server can answer -------------------
            #
            # wiretest asks for five pages. One connection per request is
            # five connections; with the connection kept it is one, or two
            # if the server recycled it. Half or fewer is the claim, rather
            # than merely "fewer", because one saved out of five would pass
            # that and would not be keep alive working.
            counts = srv.counts()
            reqs = counts["requests"]
            conns = counts["connections"]
            c.add("the server was asked for several pages", reqs >= 5)
            c.add("and did not have to accept a connection for each one",
                  conns > 0 and conns * 2 <= reqs)
            print("      %d requests over %d connection(s)" % (reqs, conns))

            # Which cookies actually travelled, as the server saw them.
            seen = srv.cookies()
            c.add("a request before anything was set carried no cookie",
                  seen and seen[0] == "")
            c.add("and one after it carried what was set",
                  any("sid=abc123" in x for x in seen))
        finally:
            vm.stop()
            try:
                os.remove(DISK)
            except OSError:
                pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
