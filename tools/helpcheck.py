"""Help: every key, narrowed as you type.

Help says on the console how many of its rows are shown and the first
("help: 1 of 70 shown; the first, files ctrl+K: make a zip of what is
chosen"). It is opened the way a person would: the launcher (the name on
the dock), "help" typed into its search, and Return.

  - opened from the launcher, it shows them all;
  - "zip" leaves one row, Files' ctrl+K;
  - "alt+" leaves the desktop's alt keys, the first alt+Tab;
  - a word nothing has leaves none;
  - and each program it covers has rows (two at least) when its name is
    typed: none was left out.

  python tools/helpcheck.py [--keep]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from deskcheck import DOCK_BADGE                             # noqa: E402

DISK = os.path.join(ROOT, "helpcheck.%d.img" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "+": "shift-equal", "\n": "ret"}
PROGRAMS = ["desktop", "terminal", "files", "notes", "sheet", "paint", "pictures", "music", "calendar", "clock",
            "browser"]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("help")
    vm = Guest(DISK, memory=256, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "help-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def keys(text):
            for ch in text:
                key(NAMED.get(ch, ch), settle=0.08)

        def said_since(mark, pattern, timeout=20):
            end = time.time() + timeout
            while time.time() < end:
                m = re.search(pattern, vm.serial()[mark:])
                if m:
                    return m
                time.sleep(0.2)
            return None

        def settled(mark, pattern):
            """The last line of the kind once typing has stopped: each letter
            typed says the rows it leaves, and the word's are the last."""
            time.sleep(1.5)
            found = list(re.finditer(pattern, vm.serial()[mark:]))
            return found[-1] if found else None

        mark = len(vm.serial())
        mon.click(*DOCK_BADGE)
        time.sleep(1.5)
        keys("help\n")
        m = said_since(mark, r"help: (\d+) of (\d+) shown", 30)
        total = int(m.group(2)) if m else 0
        c.add("opened from the launcher, it shows every row (%d)" % total,
              m is not None and m.group(1) == m.group(2) and total > 50)

        mark = len(vm.serial())
        keys("zip")
        m = settled(mark, r"help: (\d+) of \d+ shown(; the first, files ctrl\+K: make a zip)?")
        c.add("zip leaves Files' ctrl+K alone", m is not None and m.group(1) == "1" and m.group(2) is not None)

        for _ in range(3):
            key("backspace", settle=0.1)
        mark = len(vm.serial())
        keys("alt+")
        m = settled(mark, r"help: (\d+) of \d+ shown(; the first, desktop alt\+Tab)?")
        c.add("alt+ leaves the desktop's alt keys, alt+Tab first (%s)" % (m.group(1) if m else "-"),
              m is not None and m.group(2) is not None and 8 <= int(m.group(1)) <= 10)

        for _ in range(4):
            key("backspace", settle=0.1)
        mark = len(vm.serial())
        keys("zebra")
        m = settled(mark, r"help: (\d+) of \d+ shown")
        c.add("a word nothing has leaves none", m is not None and m.group(1) == "0")

        for _ in range(5):
            key("backspace", settle=0.1)
        found = []
        for name in PROGRAMS:
            mark = len(vm.serial())
            keys(name)
            m = settled(mark, r"help: (\d+) of \d+ shown")
            if m and int(m.group(1)) >= 2:
                found.append(name)
            for _ in range(len(name)):
                key("backspace", settle=0.08)
        missing = [p for p in PROGRAMS if p not in found]
        if missing:
            print("      | no rows for: %s" % missing)
        c.add("each program it covers has rows when its name is typed", not missing)
        key("alt-q", settle=1.0)
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
