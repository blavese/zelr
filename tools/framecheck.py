"""What a frame actually costs.

The desktop draws the whole screen into a back buffer whenever anything
changes, and it used to send all of it to the card every time as well.
Moving the pointer changes something, so moving the pointer cost a three
megabyte write across the bus and, under VMware, an instruction to the host
to look at all of it again. That is a frame rate fixed by the size of the
screen rather than by how much of it is different, which is what "a bit
laggy" was, and it is why giving the machine more of anything did not help.

It sends the bands of rows that differ now, and hands half of the comparison
to a second processor when there is one. Both of those are easy claims to
make and easy to be wrong about, so this measures them: it drives the
pointer around the desktop and then asks the kernel what the frames it drew
actually sent.

  python tools/framecheck.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "framecheck.%d.img" % os.getpid())


def stats(vm):
    """The numbers /sys/screen reports, as a dictionary."""
    out = vm.run("cat /sys/screen", timeout=20)
    got = {}
    for line in out.splitlines():
        bits = line.split()
        if len(bits) == 2 and bits[1].lstrip("-").isdigit():
            got[bits[0]] = int(bits[1])
    return got


def main():
    build_once()
    c = Checks("what a frame costs")

    # Two processors, because half the comparison is meant to go to the
    # second one and a machine with one would say nothing about that.
    vm = Guest(DISK, memory=512, extra=["-smp", "2"])
    try:
        vm.wait_boot()
        mon = vm.monitor()

        before = stats(vm)
        full = before.get("fullkib", 0)
        c.add("the screen says how big a whole frame is", full > 1000)

        vm.type("desktop\n")
        mon.wait_screen("fr-desktop",
                        lambda px, w, h: True, timeout=30)

        # The pointer, moved about. Nothing else on the desktop changes, so
        # every frame this produces is a frame whose only difference from the
        # one before it is a cursor twelve pixels wide.
        for x, y in ((300, 300), (360, 320), (420, 340), (480, 360),
                     (540, 380), (600, 400), (540, 380), (480, 360),
                     (420, 340), (360, 320)):
            mon.move_to(x, y)
        mon.screen("fr-moved")

        vm.type("\x1b")
        vm.wait_prompt(timeout=30)
        after = stats(vm)

        drew = after["frames"] - before["frames"]
        sent = after["sentkib"] - before["sentkib"]
        shared = after["shared"] - before["shared"]

        c.add("the desktop drew frames", drew > 10)
        if drew > 0:
            each = sent // drew
            print("      %d frames, %d KiB sent, %d KiB a frame against a "
                  "whole screen of %d KiB" % (drew, sent, each, full))
            print("      %d of them were shared with the other processor"
                  % shared)
            c.add("a frame sends less than the whole screen", each < full)
            c.add("and well under half of it", each * 2 < full)
            c.add("the other processor took half of some of them", shared > 0)
    finally:
        vm.stop()

    return c.report()


if __name__ == "__main__":
    sys.exit(main())
