"""The terminal's ctrl+R: back through what has been typed.

What a line ran is seen on the console: the commands typed are `notice
TEXT`, which the desktop says there as "notice: TEXT". Keys go in through
QEMU's keyboard, as a person's do.

  - ctrl+R and a word find the newest line with it, ctrl+R again the one
    before, and Return runs that one;
  - an arrow takes what was found to the line to be changed first;
  - Return with nothing found runs nothing;
  - ctrl+G lets the search go, and typing is the line's again.

  python tools/backcheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

DISK = os.path.join(ROOT, "backcheck.%d.img" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("back through the terminal's lines")
    vm = Guest(DISK, memory=256, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(300, 300)
        _, _, _, shot, up = mon.wait_screen(
            "back-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.25):
            mon.send("sendkey %s" % name, settle=settle)

        def keys(text):
            for ch in text:
                key(NAMED.get(ch, ch), settle=0.08)

        def said_since(mark, text, timeout=20):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        mark = len(vm.serial())
        for line in ("notice alpha one\n", "notice beta two\n", "notice alpha three\n"):
            keys(line)
        c.add("three lines typed and run", said_since(mark, "notice: alpha three", 30))

        mark = len(vm.serial())
        key("ctrl-r")
        keys("alpha")
        key("ctrl-r")
        key("ret")
        ran = said_since(mark, "notice: alpha one")
        c.add("ctrl+R and a word, ctrl+R again for the one before, Return runs it",
              ran and "notice: alpha three" not in vm.serial()[mark:])

        mark = len(vm.serial())
        key("ctrl-r")
        keys("beta")
        key("right")
        keys(" more\n")
        c.add("an arrow takes it to the line to change first", said_since(mark, "notice: beta two more"))

        mark = len(vm.serial())
        key("ctrl-r")
        keys("zzz")
        key("ret")
        keys("notice after\n")
        said_since(mark, "notice: after")
        out = vm.serial()[mark:]
        c.add("Return with nothing found runs nothing, and the line is the line again",
              "notice: after" in out and out.count("notice: ") == 1)

        mark = len(vm.serial())
        key("ctrl-r")
        keys("alpha")
        key("ctrl-g")
        keys("notice let go\n")
        c.add("ctrl+G lets the search go, and what is typed is the line's",
              said_since(mark, "notice: let go"))
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
