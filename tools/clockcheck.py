"""The clock: other places' times, a stopwatch, a timer that rings.

The machine is on the host's clock (harness HOST_CLOCK), in UTC. The clock
says on the console what it shows ("clock: here 14:05; London 14:05; New
York 09:05; Tokyo 23:05") and what it did; its places are a file,
/home/clocks, read back off the disk image here. Places' summers are
worked out here by their rules (the EU's and the US's), not by the host's
zone database, which Windows' Python does not have.

It is opened from the launcher with the terminal closed first, so it is
the first window and where the numbers below say (as setcheck places
Settings): the button that takes a place away is clicked there.

  - it opens on London, New York and Tokyo, each at its time now;
  - a place typed and added (Phoenix, which keeps no summer) is shown at
    its time, and kept in /home/clocks;
  - a place's button takes it away;
  - Space starts and stops the stopwatch, and it counts what passed;
  - the timer, given three seconds, says it is done and puts up a notice;
  - opened again, the places are as they were left.

  python tools/clockcheck.py [--keep]
"""
import datetime
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT, HOST_CLOCK   # noqa: E402
from readfat import Fat16                                                  # noqa: E402
from setcheck import (IN_X, IN_Y, CTX_X, CTX_Y, CTX_PAD, CTX_ITEM, CTX_CLOSE,  # noqa: E402
                      DOCK_SIDE, PANEL_Y, DOCK_H)

DISK = os.path.join(ROOT, "clockcheck.%d.img" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
UTC = datetime.timezone.utc

# clock.c's window and its rows of places: 440 wide; the first row starts
# under the time here (UI_PAD + the 20 px face + UI_PAD) and the section's
# heading (28 + 6), and each is UI_BTN_H + 4; its button is 34 wide at
# the right, inside UI_PAD.
CLOCK_W = 440
ROW0 = 8 + 20 + 8 + 28 + 6
ROW_H = 34


def take_button(i):
    return (IN_X + CLOCK_W - 8 - 17, IN_Y + ROW0 + i * ROW_H + 15)


def sunday(year, month, nth):
    """The nth Sunday of a month (0 for the last)."""
    if nth:
        d = datetime.date(year, month, 1)
        d += datetime.timedelta(days=(6 - d.weekday()) % 7)
        return d + datetime.timedelta(weeks=nth - 1)
    d = datetime.date(year + (month == 12), month % 12 + 1, 1) - datetime.timedelta(days=1)
    return d - datetime.timedelta(days=(d.weekday() + 1) % 7)


def offset(place, t):
    """Minutes east of UTC at a moment, by the rule zones.h has for it."""
    y = t.year
    if place == "London":
        a = datetime.datetime.combine(sunday(y, 3, 0), datetime.time(1), UTC)
        b = datetime.datetime.combine(sunday(y, 10, 0), datetime.time(1), UTC)
        return 60 if a <= t < b else 0
    if place == "New York":
        a = datetime.datetime.combine(sunday(y, 3, 2), datetime.time(7), UTC)
        b = datetime.datetime.combine(sunday(y, 11, 1), datetime.time(6), UTC)
        return -240 if a <= t < b else -300
    return {"Tokyo": 540, "Phoenix": -420}[place]


def at(place, t):
    return (t + datetime.timedelta(minutes=offset(place, t))).strftime("%H:%M")


def places_said(line):
    """{place: "HH:MM"} from "clock: here 14:05; London 14:05; ..."."""
    out = {}
    for part in line.split("; ")[1:]:
        name, _, when = part.rpartition(" ")
        out[name] = when
    return out


def read(path):
    try:
        return Fat16(DISK).read_file(path)
    except Exception:
        return None


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the clock")
    vm = Guest(DISK, memory=256, extra=HOST_CLOCK, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "clock-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(2)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def said_since(mark, text, timeout=20):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        def last_line(mark, prefix):
            out = vm.serial()[mark:]
            at_ = out.rfind(prefix)
            return "" if at_ < 0 else out[at_:].split("\n")[0].strip()

        def open_clock():
            # The terminal away (the desktop's menu), then the launcher,
            # narrowed by name.
            mark = len(vm.serial())
            mon.click(DOCK_SIDE + 16 + 38, PANEL_Y + DOCK_H // 2)
            time.sleep(1.5)
            for ch in "clo":
                key(ch, settle=0.12)
            key("ret", settle=0.5)
            said_since(mark, "clock: here ", 30)
            time.sleep(0.8)
            return mark

        mon.move_to(950, 200)
        mon.send("mouse_button 2", settle=0.4)
        mon.send("mouse_button 0", settle=1.2)
        mon.click(CTX_X + 60, CTX_Y + CTX_PAD + CTX_CLOSE * CTX_ITEM + CTX_ITEM // 2)
        time.sleep(2.5)

        def times_right(line, names):
            got = places_said(line)
            now = datetime.datetime.now(UTC)
            ok = True
            for n in names:
                ok = ok and got.get(n) in (at(n, now), at(n, now - datetime.timedelta(minutes=1)))
            return ok

        mark = open_clock()
        line = last_line(mark, "clock: here ")
        c.add("it opens from the launcher on London, New York and Tokyo, each at its time now (%s)" % line,
              list(places_said(line)) == ["London", "New York", "Tokyo"]
              and times_right(line, ["London", "New York", "Tokyo"]))

        mark = len(vm.serial())
        key("tab")
        for ch in "phoenix":
            key(ch, settle=0.06)
        key("ret")
        added = said_since(mark, "clock: added Phoenix")
        time.sleep(0.5)
        line = last_line(mark, "clock: here ")
        c.add("a place typed is added, at its time (%s)" % line, added and times_right(line, ["Phoenix"]))
        c.add("and kept in /home/clocks", b"America/Phoenix" in (read("/home/clocks") or b""))
        key("esc")

        mark = len(vm.serial())
        mon.click(*take_button(0))
        took = said_since(mark, "clock: took away London")
        c.add("a place's button takes it away", took and b"Europe/London" not in (read("/home/clocks") or b"x"))

        mark = len(vm.serial())
        key("spc")
        started = said_since(mark, "clock: stopwatch started")
        time.sleep(2.0)
        key("spc")
        stopped = said_since(mark, "clock: stopwatch stopped at ")
        shown = last_line(mark, "clock: stopwatch stopped at ").rpartition(" ")[2]
        try:
            m, rest = shown.split(":")
            secs = int(m) * 60 + float(rest)
        except ValueError:
            secs = -1
        c.add("Space starts and stops the stopwatch, and it counted what passed (%s)" % shown,
              started and stopped and 1.0 <= secs <= 5.0)

        mark = len(vm.serial())
        key("tab")
        key("tab")
        for k in ("0", "shift-semicolon", "0", "3"):
            key(k, settle=0.06)
        key("ret")
        began = said_since(mark, "clock: the timer started, 3 seconds")
        done = said_since(mark, "clock: the timer is done", 30)
        notice = said_since(mark, "notice: the timer is done", 10)
        c.add("the timer, given 0:03, says it is done and puts up a notice", began and done and notice)

        key("alt-q", settle=1.0)
        time.sleep(1.0)
        mark = open_clock()
        line = last_line(mark, "clock: here ")
        c.add("opened again, the places are as they were left (%s)" % line,
              list(places_said(line)) == ["New York", "Tokyo", "Phoenix"])
        key("alt-q", settle=1.0)
    finally:
        vm.stop()
        if not keep and os.path.exists(DISK):
            os.remove(DISK)
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
