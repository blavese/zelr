"""The time zone, from the shell and from Settings' Time page.

The desktop's clock showed the CMOS clock as it was, which is UTC: the wrong
hour for nearly everybody. A zone is a line in /zelr.cfg now (include/zone.h),
set with the shell's zone command or on Settings' Time page, and /sys/time's
local time, the shell's date and the dock follow it. This sets one from the
shell and checks /sys/time's local time against its UTC, then chooses
another in Settings with the mouse and reads the file and the date back.
The zones have no summer time, so nothing here depends on the date it runs
on, and half and three quarter hours, so the minutes are checked too.

Settings says on the console what its Time page shows ("settings: time zone
..."), as its Wireless page does, and that is what each click waits for.

  python tools/timecheck.py [--keep]
"""
import datetime
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT                       # noqa: E402
from setcheck import (PAGES, IN_X, IN_Y, PAGE_X, UI_PAD, UI_ROW, CTX_X, CTX_Y,  # noqa: E402
                      CTX_PAD, CTX_ITEM, CTX_CLOSE, DOCK_SIDE, DOCK_H, PANEL_Y)

DISK = os.path.join(ROOT, "timecheck.%d.img" % os.getpid())

# The dock's clock, at its right hand end.
CLOCK = (920, PANEL_Y + 6, 1004, PANEL_Y + DOCK_H - 6)


def crop(px, w, rect):
    left, top, right, bottom = rect
    return b"".join(px[(y * w + left) * 3:(y * w + right) * 3] for y in range(top, bottom))

# The Time page as settings.c lays it out: a section heading is 34 high, the
# time and the zone below it 22 and 30, then the list's heading, then rows of
# 28. The list opens at the zone in use.
UI_TITLE = 34
LIST_Y = IN_Y + UI_PAD + UI_TITLE + 22 + 30 + UI_TITLE


def sidebar(name):
    i = PAGES.index(name)
    return (IN_X + 80, IN_Y + UI_PAD + i * (UI_ROW + 2) + UI_ROW // 2)


def zone_row(i):
    return (PAGE_X + 100, LIST_Y + i * (UI_ROW + 2) + UI_ROW // 2)


def stamp(text, key):
    m = re.search(r"^%s\s+(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)" % key, text, re.M)
    return datetime.datetime(*map(int, m.groups())) if m else None


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("time zones")
    vm = Guest(DISK, memory=256)
    try:
        vm.wait_boot()

        out = vm.fresh("zone")
        c.add("a machine nobody has told is in UTC", "zone UTC" in out)
        out = vm.fresh("zone Mars/Olympus")
        c.add("a zone it does not know is refused", "is not one this knows" in out)
        out = vm.fresh("zone asia/calcutta")
        c.add("a zone is set from the shell, by its name in any case",
              "in Asia/Calcutta (UTC+5:30)" in out)

        st = vm.fresh("cat /sys/time")
        now, local = stamp(st, "now"), stamp(st, "local")
        gap = (local - now).total_seconds() if now and local else None
        c.add("/sys/time's local time is five and a half hours on from its UTC (%s)" % gap,
              gap is not None and abs(gap - 330 * 60) <= 2)
        c.add("and it says the zone and the offset",
              "zone      Asia/Calcutta" in st and "offset    330" in st)
        cfg = vm.fresh("cat /zelr.cfg")
        c.add("the shell writes the zone into /zelr.cfg", "zone Asia/Calcutta" in cfg)

        # --- Settings' Time page ---------------------------------------------
        def said_since(mark, text, timeout):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        def click_until(at, text, timeout=20, tries=3):
            for _ in range(tries):
                mark = len(vm.serial())
                mon.click(*at)
                if said_since(mark, text, timeout):
                    return True
            return False

        vm.type("desktop\n")
        mon = vm.monitor()
        mon.wait_screen("time-desk", lambda w, h, px: True, timeout=60)
        time.sleep(3)
        # The terminal away, then Settings from the launcher, by name, so it
        # is the first window and lands where setcheck.py's numbers say.
        mon.move_to(950, 200)
        mon.send("mouse_button 2", settle=0.4)
        mon.send("mouse_button 0", settle=1.2)
        mon.click(CTX_X + 60, CTX_Y + CTX_PAD + CTX_CLOSE * CTX_ITEM + CTX_ITEM // 2)
        time.sleep(2.5)
        mon.click(DOCK_SIDE + 16 + 38, PANEL_Y + DOCK_H // 2)
        time.sleep(1.5)
        for ch in "sett":
            mon.send("sendkey %s" % ch, settle=0.12)
        mon.send("sendkey ret", settle=0.5)
        time.sleep(4)

        shown = click_until(sidebar("Time"), "settings: time zone Asia/Calcutta, UTC+5:30", timeout=15)
        c.add("Settings has a Time page, and it shows the zone the shell set", shown)
        if shown:
            # The list opens at Calcutta; the row under it is the next in the
            # table, Kathmandu, a quarter of an hour on. The dock's clock is
            # read before and after, early in a minute by the host's clock
            # (which the machine's CMOS clock follows), so a change seen is
            # the zone's and not the minute turning over.
            while not 2 <= time.gmtime().tm_sec <= 15:
                time.sleep(0.2)
            minute = time.gmtime().tm_min
            w, h, px, _, _ = mon.wait_screen("time-calcutta", lambda w, h, px: True)
            before = crop(px, w, CLOCK)
            chosen = click_until(zone_row(1), "settings: time zone Asia/Kathmandu, UTC+5:45", timeout=15)
            c.add("a zone chosen in the list is the machine's zone a moment later", chosen)
            _, _, _, shot, moved = mon.wait_screen(
                "time-dock", lambda w, h, px: crop(px, w, CLOCK) != before, timeout=10)
            c.add("and the dock's clock moves to it",
                  moved and time.gmtime().tm_min == minute, shot)

        # The file and the date, from the console: escape hands it back.
        mon.send("sendkey esc", settle=1.0)
        time.sleep(2.0)
        vm.wait_prompt(timeout=30)
        cfg = vm.fresh("cat /zelr.cfg")
        c.add("Settings wrote the zone into /zelr.cfg, once",
              cfg.count("zone ") == 1 and "zone Asia/Kathmandu" in cfg)
        out = vm.fresh("date")
        c.add("and the shell's date is told in it", "in Asia/Kathmandu (UTC+5:45)" in out)
    finally:
        vm.stop()

    code = c.report(keep=keep)
    if not keep:
        try:
            os.remove(DISK)
        except OSError:
            pass
    return code


if __name__ == "__main__":
    sys.exit(main())
