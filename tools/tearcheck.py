"""Whether a window the desktop shows is a frame that was finished.

A window's pixels used to be the pixels the program was still writing.
win_commit marked the window dirty and swapped nothing, so there was no
moment at which a frame became finished: a program whose frame begins by
painting over everything contained a background and nothing else for as long
as that took, and the compositor -- a task like any other -- ran in the gap.

It is not a fault a screenshot of a real program can prove, because the gap
is short and whether a sample lands inside it is luck. So `halfdrawn` makes
the gap enormous: it paints the whole window red, waits a fifth of a second
without committing, then paints it green and commits.

Red is therefore a colour that program never publishes. Every sample of the
screen that contains red is a frame the desktop read before it was finished.

  python tools/tearcheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_all, ROOT   # noqa: E402

DISK = os.path.join(ROOT, "tearcheck.%d.img" % os.getpid())

# The three colours halfdrawn uses, exactly as it writes them.
RED   = (0xC8, 0x1E, 0x1E)
GREEN = (0x1E, 0x96, 0x32)
MINT  = (0x1E, 0xC8, 0x64)

SAMPLES = 14


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("finished frames")

    vm = Guest(DISK, memory=256)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        time.sleep(7)
        mon = vm.monitor()

        vm.type("halfdrawn\n")
        time.sleep(5)
        mon.move_to(20, 20)
        time.sleep(1.0)

        # --- is it even up? ------------------------------------------------
        w, h, px, shot = mon.screen("tear-up")
        first = count_all(px, GREEN) + count_all(px, MINT)
        c.add("the window is on the screen", first > 4000, shot)
        if first <= 4000:
            print("      %d green pixels" % first)

        # --- and never caught mid-frame ------------------------------------
        #
        # Sampled at an interval that is not a multiple of the program's, so
        # the samples walk through its cycle rather than landing on the same
        # point of it every time. A check that sampled in step with the
        # program could miss the red half entirely and prove nothing.
        worst_red = 0
        shades = set()
        for i in range(SAMPLES):
            time.sleep(0.13 + 0.01 * i)
            mon.move_to(20 + (i % 3), 20)
            w, h, px, shot = mon.screen("tear-%d" % i)
            red = count_all(px, RED)
            if red > worst_red:
                worst_red = red
            g, m = count_all(px, GREEN), count_all(px, MINT)
            if g > 4000: shades.add("green")
            if m > 4000: shades.add("mint")

        c.add("and no sample of it was a frame that had not been finished",
              worst_red == 0, shot)
        if worst_red:
            print("      %d red pixels in the worst sample, of a colour "
                  "that is never committed" % worst_red)

        # Without this the check above passes against a window that stopped
        # drawing altogether, which shows no red for the same reason a black
        # rectangle shows no red.
        c.add("while the window was still drawing new frames",
              len(shades) == 2, shot)
        if len(shades) != 2:
            print("      only saw: %s" % (", ".join(sorted(shades)) or "nothing"))
    finally:
        vm.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
