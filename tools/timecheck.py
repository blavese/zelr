"""The time zone, from the shell and from Settings' Time page.

The desktop's clock showed the CMOS clock as it was, which is UTC: the wrong
hour for nearly everybody. A zone is a line in /zelr.cfg now (include/zone.h),
set with the shell's zone command or on Settings' Time page, and /sys/time's
local time, the shell's date and the dock follow it. This sets one from the
shell and checks /sys/time's local time against its UTC, then chooses
another in Settings with the mouse and reads the file and the date back.
The zones have no summer time, so nothing here depends on the date it runs
on, and half and three quarter hours, so the minutes are checked too.

And a file's times, which were the first of January 2025 for every file
whatever the clock said: one written from the shell is stamped with the
host's time in the zone, read off the disk image here (tools/readfat.py),
and `ls -l` shows it, in the shell, in the terminal (its scrollback copied
to the clipboard and read back through /sys/clipboard) and in Files (which
says on the console what its status line shows).

And the console, which drew a program's printing over the desktop and, at
the bottom of the screen, scrolled the whole desktop up (fbcon.h
fbcon_hold). Settings prints a line each time the zone changes, so the zone
is moved back and forth fourteen times: past the twelve lines a console at
any row prints before it scrolls. Then nowhere on the screen may be the
console's ground, which the desktop never draws in.

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
from harness import Guest, Checks, build_once, count_in, ROOT             # noqa: E402
from readfat import Fat16                                                 # noqa: E402
from setcheck import (PAGES, IN_X, IN_Y, PAGE_X, UI_PAD, UI_ROW, CTX_X, CTX_Y,  # noqa: E402
                      CTX_PAD, CTX_ITEM, CTX_CLOSE, DOCK_SIDE, DOCK_H, PANEL_Y)

DISK = os.path.join(ROOT, "timecheck.%d.img" % os.getpid())

TERMINAL = (0x10, 0x14, 0x1A)          # the terminal's default background
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}

# The console's ground (fbcon.c PALETTE[0]): behind every character it draws,
# and what a scroll fills the bottom quarter of the screen with.
CONSOLE = (0x14, 0x18, 0x1D)
SCREEN = (0, 0, 1024, 768)

# The dock's clock, at its right hand end.
CLOCK = (920, PANEL_Y + 6, 1004, PANEL_Y + DOCK_H - 6)


def crop(px, w, rect):
    left, top, right, bottom = rect
    return b"".join(px[(y * w + left) * 3:(y * w + right) * 3] for y in range(top, bottom))

# The Time page as settings.c lays it out: a section heading is 34 high, the
# time and the zone below it 22 and 30, the clock's two switches 30 each, what
# the time server said 22 and a gap of 6, then the list's heading, then rows
# of 28. The list opens at the zone in use.
UI_TITLE = 34
LIST_Y = IN_Y + UI_PAD + UI_TITLE + 22 + 30 + 30 + 30 + 22 + 6 + UI_TITLE


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

        # --- a file's times --------------------------------------------------
        before = datetime.datetime.utcnow()
        vm.fresh("write /home/when.txt stamped")
        after = datetime.datetime.utcnow()
        try:
            stamps = Fat16(DISK).times("/home/when.txt")
        except Exception:
            stamps = None
        written = stamps[0] if stamps else None
        zone = datetime.timedelta(minutes=330)
        c.add("a file written is stamped with the time in the zone (%s, the host %s to %s there)"
              % (written, (before + zone).strftime("%H:%M:%S"), (after + zone).strftime("%H:%M:%S")),
              written is not None and before + zone - datetime.timedelta(seconds=3) <= written
              <= after + zone + datetime.timedelta(seconds=1))
        c.add("and with when it was made, the same moment", stamps is not None and stamps[1] == written)
        out = vm.fresh("ls -l /home")
        c.add("ls -l says when it was written",
              written is not None and written.strftime("%Y-%m-%d %H:%M") + "  when.txt" in out)

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
        _, _, _, _, up = mon.wait_screen(
            "time-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        time.sleep(1)                      # the terminal takes its first key

        def keys(text):
            for ch in text:
                mon.send("sendkey %s" % NAMED.get(ch, ch), settle=0.06)

        # The terminal's ls -l, read back by way of the clipboard: everything
        # on its screen selected and copied, and /bin/cat, run from it,
        # prints to the console.
        when = written.strftime("%Y-%m-%d %H:%M") + "  when.txt" if written else "?"
        keys("clear\nls -l /home\n")
        time.sleep(1)
        mon.send("sendkey ctrl-a", settle=0.3)
        mon.send("sendkey ctrl-c", settle=0.3)
        mark = len(vm.serial())
        keys("/bin/cat /sys/clipboard\n")
        c.add("the terminal's ls -l says when it was written", said_since(mark, when, 20))

        # Files on /home, the selection moved down until it is the file.
        mark = len(vm.serial())
        keys("/bin/files /home\n")
        files_said = said_since(mark, "files: /home", 30)
        found = False
        for _ in range(6):
            if not files_said:
                break
            mon.send("sendkey down", settle=0.5)
            if said_since(mark, "files: when.txt", 3):
                found = True
                break
        line = ""
        for ln in vm.serial()[mark:].splitlines():
            if ln.startswith("files: when.txt"):
                line = ln.strip()
        told = ("%d %s %d, %02d:%02d" % (written.day, written.strftime("%B"), written.year,
                                        written.hour, written.minute)) if written else "?"
        c.add("and Files says so beside the file chosen (%s)" % line, found and line.endswith("written " + told))
        mon.send("sendkey alt-q", settle=1.0)
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
            # Back and forth, ending where it was: a line printed each time.
            said = 0
            for i in range(14):
                name = "Asia/Calcutta, UTC+5:30" if i % 2 == 0 else "Asia/Kathmandu, UTC+5:45"
                said += click_until(zone_row(0 if i % 2 == 0 else 1), "settings: time zone " + name, timeout=15)
            w, h, px, shot, _ = mon.wait_screen("time-page", lambda w, h, px: True)
            ground = count_in(px, w, SCREEN, CONSOLE)
            c.add("and the console printed nothing over the desktop (%d lines, %d pixels of its ground)"
                  % (said, ground), said == 14 and ground < 64, shot)

        # The file and the date, from the console: escape hands it back.
        mon.send("sendkey esc", settle=1.0)
        time.sleep(2.0)
        vm.wait_prompt(timeout=30)
        cfg = vm.fresh("cat /zelr.cfg")
        c.add("Settings wrote the zone into /zelr.cfg, once",
              cfg.count("zone ") == 1 and "zone Asia/Kathmandu" in cfg)
        out = vm.fresh("date")
        c.add("and the shell's date is told in it", "in Asia/Kathmandu (UTC+5:45)" in out)

        # A clock that keeps local time. QEMU's keeps UTC, so with the switch
        # on its reading is taken as Kathmandu's wall clock: /sys/time's
        # local time is then the host's UTC, and its UTC 345 minutes before.
        vm.fresh("write /zelr.cfg clock_local 1")
        host = datetime.datetime.utcnow()
        # date before anything reads /sys/time, which reads the file itself:
        # this is the shell picking up what a command wrote.
        out = vm.fresh("date")
        told = re.search(r", (\d\d):(\d\d):(\d\d) in Asia/Kathmandu", out)
        shown = (int(told.group(1)) * 3600 + int(told.group(2)) * 60 + int(told.group(3))) if told else None
        at = host.hour * 3600 + host.minute * 60 + host.second
        c.add("a clock switched to local time from the shell is read so at once (date says %s, the host %s)"
              % (told.group(0)[2:10] if told else None, host.strftime("%H:%M:%S")),
              shown is not None and min(abs(shown - at), 86400 - abs(shown - at)) <= 3)
        host = datetime.datetime.utcnow()
        st = vm.fresh("cat /sys/time")
        now, local = stamp(st, "now"), stamp(st, "local")
        c.add("a clock kept in local time is read as the zone's wall clock (%s, the host %s)"
              % (local, host.strftime("%H:%M:%S")),
              "clock     local" in st and local is not None and abs((local - host).total_seconds()) <= 3
              and now is not None and (local - now).total_seconds() == 345 * 60)
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
