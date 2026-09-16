"""Input that still works after somebody touched the machine while it booted.

Every other check in this project starts a machine, waits for it to settle,
and only then types at it. Nobody does that. People press keys while a
machine is starting and move the mouse over the window, and on a virtual
machine they click into it to give it focus, which is a mouse event.

That distinction was worth a bug. The 8042 hands over one byte at a time and
will not deliver another until the last has been read, and it raises its
interrupt on the edge of a byte arriving. A byte that turned up while the
line was still masked leaves the controller holding it with nobody coming,
and from that moment the keyboard and the mouse are both dead, because they
share the buffer. It took one keypress during boot to arrange, it survived
every other check in this project, and what it looked like from the outside
was zelr working on one machine and not on the next.

  python tools/inputcheck.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

NAMED = {" ": "spc", "\n": "ret"}


def keys(mon, text, settle=0.06):
    for ch in text:
        mon.send("sendkey %s" % NAMED.get(ch, ch), settle=settle)


def works(presses=0, moves=0):
    """Starts a machine, interferes with it on the way up, then types."""
    disk = os.path.join(ROOT, "inputcheck.%d.img" % os.getpid())
    vm = Guest(disk, memory=128, machine="q35")
    try:
        mon = vm.monitor()
        for _ in range(presses):
            keys(mon, "a")
        for _ in range(moves):
            mon.send("mouse_move 5 5", settle=0.05)
            mon.send("mouse_move -5 -5", settle=0.05)

        vm.wait_boot()

        # Through the monitor, so this is the PS/2 keyboard and not the
        # serial line the rest of the tests drive the shell with. A key that
        # arrives here came through the controller and the interrupt path.
        marks = vm.prompts()
        keys(mon, "uname\n")
        return vm.wait_prompt(marks + 1, timeout=25)
    finally:
        vm.stop()
        try:
            os.remove(disk)
        except OSError:
            pass


def main():
    build_once()
    c = Checks("input test")

    c.add("a machine nobody touched can be typed on", works())
    c.add("and one that had keys pressed while it started",
          works(presses=8))
    c.add("and one that had a lot of them", works(presses=25))
    c.add("and one whose mouse was moved while it started",
          works(moves=8))
    c.add("and one that had both", works(presses=8, moves=8))

    return c.report()


if __name__ == "__main__":
    sys.exit(main())
