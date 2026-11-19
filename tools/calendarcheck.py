"""The calendar: today, moving about, and a note kept for a day.

The machine is on the host's clock (harness HOST_CLOCK, no time server), in
UTC, so today in it is today here in UTC. The calendar says on the console
what it shows ("calendar: 2026-11-20, Friday, November 2026", and the day's note
after a colon), and its notes are a file, /home/calendar, read back off the
disk image here (tools/readfat.py).

  - it opens on today, as the host's clock has it;
  - the right arrow is tomorrow, Page Down a month on, Home today again;
  - Return, a note typed, Return: the note is kept, in the file as
    "YYYY-MM-DD<tab>NOTE";
  - closed and opened again, the note is still there;
  - Delete takes it away, and the file no longer has it;
  - every day said is said with its weekday, as Python's calendar has it;
  - the days of the week come out right across a leap year: 29 February
    2028 is reached from 28 February by the right arrow, and the 1st of
    March after it by one more.

  python tools/calendarcheck.py [--keep]
"""
import datetime
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT, HOST_CLOCK   # noqa: E402
from readfat import Fat16                                                  # noqa: E402

DISK = os.path.join(ROOT, "calendarcheck.%d.img" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}
MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September",
          "October", "November", "December"]


DAYS = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]


def shown(d):
    """As the calendar says it, its weekday Python's own (date.weekday())."""
    return "calendar: %04d-%02d-%02d, %s, %s %d" % (d.year, d.month, d.day, DAYS[d.weekday()], MONTHS[d.month - 1],
                                                     d.year)


def month_on(d):
    y, m = (d.year + 1, 1) if d.month == 12 else (d.year, d.month + 1)
    last = [31, 29 if (y % 4 == 0 and y % 100 != 0) or y % 400 == 0 else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][m - 1]
    return datetime.date(y, m, min(d.day, last))


def read(path):
    try:
        return Fat16(DISK).read_file(path)
    except Exception:
        return None


def wait_disk(test, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if test():
            return True
        time.sleep(0.5)
    return test()


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the calendar")
    vm = Guest(DISK, memory=256, extra=HOST_CLOCK, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "cal-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def said_since(mark, text, timeout=20):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        def open_calendar():
            mark = len(vm.serial())
            for ch in "/bin/calendar\n":
                key(NAMED.get(ch, ch), settle=0.06)
            return mark

        # Today, as the host has it in UTC (either side of midnight, should
        # the check be run across one).
        before = datetime.datetime.now(datetime.timezone.utc).date()
        mark = open_calendar()
        said_since(mark, "calendar: ", 30)
        time.sleep(0.5)
        after = datetime.datetime.now(datetime.timezone.utc).date()
        out = vm.serial()[mark:]
        today = before if shown(before) in out else after
        c.add("it opens on today, as the host's clock has it (%s)" % today, shown(today) in out)

        mark = len(vm.serial())
        key("right")
        c.add("the right arrow is the next day", said_since(mark, shown(today + datetime.timedelta(days=1))))
        mark = len(vm.serial())
        key("pgdn")
        c.add("Page Down is a month on", said_since(mark, shown(month_on(today + datetime.timedelta(days=1)))))
        mark = len(vm.serial())
        key("home")
        c.add("Home is today again", said_since(mark, shown(today)))

        # A note for today.
        mark = len(vm.serial())
        key("ret")
        for ch in "dentist at ten":
            key(NAMED.get(ch, ch), settle=0.05)
        key("ret")
        kept = said_since(mark, "calendar: kept a note for %s" % today.isoformat())
        line = "%s\tdentist at ten\n" % today.isoformat()
        c.add("Return, a note and Return keep it, in /home/calendar as a line for the day",
              kept and wait_disk(lambda: line.encode() in (read("/home/calendar") or b"")))
        c.add("and it is shown as the day's", said_since(mark, shown(today) + ": dentist at ten"))

        key("alt-q", settle=1.0)
        time.sleep(1.0)
        mark = open_calendar()
        c.add("closed and opened again, the note is still there",
              said_since(mark, shown(today) + ": dentist at ten", 30))

        mark = len(vm.serial())
        key("delete")
        c.add("Delete takes it away, and the file no longer has it",
              said_since(mark, "calendar: took the note away from %s" % today.isoformat())
              and wait_disk(lambda: line.encode() not in (read("/home/calendar") or b"x")))

        # Across a leap day: from today to 28 February 2028 by months and
        # days, then a day and a day more.
        target = datetime.date(2028, 2, 28)
        d = today
        months = 0
        while (d.year, d.month) < (target.year, target.month):
            d = month_on(d)
            months += 1
        for _ in range(months):
            key("pgdn", settle=0.08)
        for _ in range(abs(target.day - d.day)):
            key("right" if target.day > d.day else "left", settle=0.08)
        mark = len(vm.serial())
        key("right")
        leapday = said_since(mark, shown(datetime.date(2028, 2, 29)))
        key("right")
        march = said_since(mark, shown(datetime.date(2028, 3, 1)))
        c.add("2028 has a 29th of February, and the 1st of March after it", leapday and march)
        key("alt-q", settle=1.0)
    finally:
        vm.stop()
        if not keep and os.path.exists(DISK):
            os.remove(DISK)
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
