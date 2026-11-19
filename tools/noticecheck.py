"""The notices given, kept and listed with alt+N.

Notices are made with the terminal's `notice TEXT`, which the desktop says
on the console ("notice: TEXT"). alt+N opens the list and the desktop says
what it holds, newest first ("desktop: notices: 2: B | A"); the panel is
looked for on the screen, beside the dock and clear of where the notice
itself is drawn.

  - two notices are listed, newest first, and the panel is drawn;
  - Escape puts it away, and the screen is as it was;
  - they are still there once the notices themselves have gone;
  - twelve are kept, the oldest let go.

  python tools/noticecheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

DISK = os.path.join(ROOT, "noticecheck.%d.img" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
PANEL = (700, 600, 1000, 700)          # the panel's right part, right of any notice drawn


def patch(px, w, rect):
    left, top, right, bottom = rect
    return b"".join(px[(y * w + left) * 3:(y * w + right) * 3] for y in range(top, bottom))


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("notices kept")
    vm = Guest(DISK, memory=256, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(300, 300)
        _, _, _, shot, up = mon.wait_screen(
            "notice-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def said_since(mark, text, timeout=20):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        mark = len(vm.serial())
        vm.type("notice the first one\n")
        first = said_since(mark, "notice: the first one")
        vm.type("notice and a second\n")
        c.add("the terminal's notice puts one up", first and said_since(mark, "notice: and a second"))

        w, h, px, _, _ = mon.wait_screen("notice-before", lambda w, h, px: True)
        before = patch(px, w, PANEL)
        mark = len(vm.serial())
        mon.send("sendkey alt-n", settle=0.5)
        listed = said_since(mark, "desktop: notices: 2: and a second | the first one")
        _, _, _, shot, drawn = mon.wait_screen(
            "notice-open", lambda w, h, px: patch(px, w, PANEL) != before, timeout=20)
        c.add("alt+N lists both, newest first, in a panel over the desktop", listed and drawn, shot)

        mark = len(vm.serial())
        mon.send("sendkey esc", settle=0.5)
        gone = said_since(mark, "desktop: notices put away")
        _, _, _, shot, back = mon.wait_screen(
            "notice-closed", lambda w, h, px: patch(px, w, PANEL) == before, timeout=20)
        c.add("Escape puts it away, and that part of the screen is as it was", gone and back, shot)

        # The notices themselves are up for four seconds; the list keeps them.
        time.sleep(6)
        mark = len(vm.serial())
        mon.send("sendkey alt-n", settle=0.5)
        c.add("they are still listed once the notices have gone",
              said_since(mark, "desktop: notices: 2: and a second | the first one"))
        mon.send("sendkey alt-n", settle=0.5)

        mark = len(vm.serial())
        for i in range(1, 13):
            vm.type("notice n%d\n" % i)
        said_since(mark, "notice: n12", 60)
        mark = len(vm.serial())
        mon.send("sendkey alt-n", settle=0.5)
        # The count says twelve; the line's ending is left out (serial sends a CR before it).
        want = "desktop: notices: 12: " + " | ".join("n%d" % i for i in range(12, 0, -1))
        ok = said_since(mark, want)
        if not ok:
            print("      | " + " / ".join(l for l in vm.serial()[mark:].splitlines() if "notices" in l))
        c.add("twelve are kept, the two oldest let go", ok)
    finally:
        vm.stop()
    if not keep:
        try:
            os.remove(DISK)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
