"""A network call that nobody answers, and a machine that carries on anyway.

Every wait in the network stack used to spin inside the kernel until the
clock passed a deadline. Inside a system call interrupts are off, the clock
only moves on an interrupt, and so the deadline never came: a peer that did
not answer stopped the whole machine for good. docs/atlas/repro/nethang.py is
where that was first shown, on a network with no way out; this is the check
that keeps it fixed, on two networks:

  an ordinary one, with a server on the host that takes the connection and
  then never sends a byte -- a hung web server, which is the everyday case.
  /bin/hangtest waits on it from one program while another carries on, stops
  the waiting one in the middle of its call, and then checks the network and
  its sockets survived that. It also tries to end the kernel's own tasks,
  which a program must not be able to do.

  one where nothing outside answers at all (QEMU's restrict=on), so the wait
  is for an address that never resolves. A program asking from the
  background must not take the shell with it.

Before the fix the first network never printed past hangtest's opening
lines and the second never gave the prompt back.

  python tools/hangcheck.py [--keep]
"""
import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT          # noqa: E402
from webserver import Server, HOST_IP                        # noqa: E402

DISK = os.path.join(ROOT, "hangcheck.%d.img" % os.getpid())
NOWHERE = "93.184.216.34"


class Silent:
    """Takes every connection and says nothing on any of them, which is what
    a server that has hung looks like from the other end."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.held = []
        self.thread = threading.Thread(target=self._take, daemon=True)

    def _take(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            self.held.append(c)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.sock.close()
        for c in self.held:
            try:
                c.close()
            except OSError:
                pass


def ordinary(c):
    with Server() as srv, Silent() as silent:
        vm = Guest(DISK, memory=256, extra=srv.qemu_args())
        try:
            vm.wait_boot()
            vm.run("dhcp", timeout=25)
            out = vm.fresh("exec /bin/hangtest %s %d %d" % (HOST_IP, silent.port, srv.port),
                           timeout=240)
            c.add("hangtest ran to the end while a program waited on a silent server",
                  "HANGTEST_PASS" in out or "HANGTEST_FAIL" in out)
            for line in out.split("\n"):
                s = line.strip()
                if s.startswith("ok ") or s.startswith("FAIL "):
                    c.add(s.split(" ", 1)[1].strip(), s.startswith("ok "))
            c.add("the silent server really was connected to", len(silent.held) >= 1)
        finally:
            vm.stop()


def nowhere(c):
    extra = ["-netdev", "user,id=n0,restrict=on", "-device", "e1000,netdev=n0"]
    vm = Guest(DISK, memory=256, extra=extra)
    try:
        vm.wait_boot()
        vm.run("dhcp", timeout=25)
        vm.run("bg /bin/wiretest http://%s:80" % NOWHERE, timeout=20)
        # Long enough that the program is well inside a wait: ARP for an
        # address with no gateway takes seconds to give up.
        time.sleep(3)
        want = vm.prompts() + 1
        vm.type("echo still-here\n")
        c.add("with nothing outside answering, the shell still answers",
              vm.wait_prompt(want, timeout=30))
        # And the program itself comes back with an answer rather than never.
        c.add("and the program waiting gives up and says so",
              vm.wait_serial("WIRETEST_", timeout=180))
    finally:
        vm.stop()


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("a network nobody answers")
    try:
        ordinary(c)
        nowhere(c)
    finally:
        try:
            os.remove(DISK)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
