"""Drives the settings and checks the machine actually changed.

Every number the desktop is drawn from is a line in /zelr.cfg now, and the
list of them lives in one table in kernel/theme.c. Three things have to be
true and none of them is obvious from reading either side:

  the kernel describes its own settings, with a range and a default;
  a key written into the file moves what is on the screen; and
  the settings window writes the same keys the kernel parses.

The third is the one that used to be wrong. The window kept its own copy of
the list, and a key it did not know about was a key it deleted from the
machine every time it saved. So the check here is not that the window drew a
control: it is that clicking the control changed the desktop behind it.

  python tools/setcheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Guest, Checks, build_once, count_near, row_mean,
                     colour_gap, ROOT)                      # noqa: E402

DISK = os.path.join(ROOT, "setcheck.%d.img" % os.getpid())

SCREEN_W, SCREEN_H = 1024, 768

# The dock at its default, and the band it would occupy if it were twice as
# tall. Nothing is in that band but wallpaper until it is.
DOCK_H, DOCK_GAP, DOCK_SIDE = 44, 14, 16
PANEL_Y = SCREEN_H - DOCK_H - DOCK_GAP
TALL = 96
TALL_Y = SCREEN_H - TALL - DOCK_GAP

# Somewhere along the dock with nothing drawn on it: past the name badge and
# well short of the field in the middle.
PROBE_X = 260

# The badge itself, which is the thing "Show the name" switches off.
#
# The middle of it rather than the whole of it. With the name off, the first
# window chip slides along into the space the badge was using, and the bar
# under whichever window is in front is drawn in the accent at nearly full
# strength: counting the accent over the badge's whole height found that bar
# and reported a badge that was plainly not on the screen. The bar is in the
# bottom three pixels and the name is not.
BADGE = (DOCK_SIDE + 16, PANEL_Y + 10, DOCK_SIDE + 16 + 76, PANEL_Y + 30)

# The settings window as the first one of the session, and the geometry of
# its Everything page. Written the way the program writes it rather than as
# numbers that came out of it once.
WIN_X, WIN_Y = 124, 36
WM_BORDER, WM_TOP = 1, 33
IN_X, IN_Y = WIN_X + WM_BORDER, WIN_Y + WM_TOP

SIDEBAR_W, UI_PAD, UI_ROW = 160, 8, 26
PAGE_X = IN_X + SIDEBAR_W + UI_PAD * 2
ROW_H, NAME_W = 30, 172

PAGES = ["Colours", "Dock", "Windows", "Desktop", "Behaviour",
         "Screen", "Everything", "The file", "System", "About"]

# The desktop's own menu, which is how the terminal is cleared away before
# the settings window is opened: the cascade has to start from empty or the
# window is not where the arithmetic above says.
CTX_W, CTX_ITEM, CTX_PAD = 204, 30, 8
CTX_X, CTX_Y = SCREEN_W - CTX_W - 4, 200
CTX_CLOSE = 4


def sidebar(name):
    i = PAGES.index(name)
    return (IN_X + 80, IN_Y + UI_PAD + i * (UI_ROW + 2) + UI_ROW // 2)


def every_toggle(index):
    """The switch on row `index` of the Everything page, whose order is the
    order /sys/settings lists them in."""
    return (PAGE_X + NAME_W + 20,
            IN_Y + UI_PAD + index * ROW_H + ROW_H // 2)


def table_rows(text, command):
    """The lines a command printed, and only those.

    Everything the machine has said since it booted is in this buffer,
    including a welcome banner whose sentences have six words in them. Read
    as a table those are rows with a name and five numbers that are not
    numbers, and the check that every setting has a sane range then failed
    on a line of English prose."""
    at = text.rfind(command)
    if at < 0:
        return []
    body = text[at + len(command):]
    end = body.find("zelr:")
    if end >= 0:
        body = body[:end]

    rows = []
    for ln in body.split("\n"):
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        parts = ln.split()
        if len(parts) >= 6:
            rows.append(parts)
    return rows


def main():
    keep = "--keep" in sys.argv
    build_once()
    vm = Guest(DISK, memory=256)
    c = Checks("settings test")
    shots = []

    try:
        vm.wait_boot()

        # --- the kernel describes itself ----------------------------------
        table = vm.run("cat /sys/settings", timeout=20)
        rows = table_rows(table, "cat /sys/settings")

        c.add("the kernel lists its own settings", len(rows) >= 25)

        # value, low, high and default, and the value inside the range.
        sane = True
        for r in rows:
            try:
                v, lo, hi, d = (int(r[1]), int(r[2]), int(r[3]), int(r[4]))
            except ValueError:
                sane = False
                break
            if not (lo <= v <= hi and lo <= d <= hi and lo < hi):
                sane = False
                break
        c.add("every one has a range, a default and a value inside it", sane)

        keys = set(r[0] for r in rows)
        c.add("the dock, the windows and the desktop are all in it",
              {"dock_h", "dock_clock", "title_h", "border", "icon_size",
               "wallpaper", "anim_ms", "volume"} <= keys)

        # The palette is not in that list, and is offered separately.
        theme = vm.run("cat /sys/theme", timeout=20)
        c.add("and the palette is offered as colours rather than numbers",
              "accent 0x" in theme and "surface 0x" in theme)

        # --- the desktop, before anything is changed ----------------------
        vm.type("desktop\n")
        time.sleep(8)
        mon = vm.monitor()
        mon.move_to(980, 300)
        time.sleep(1.0)

        w, h, px, shot, _ = mon.wait_screen("set-before", lambda w, h, px: True)
        shots.append(shot)
        above = row_mean(px, w, TALL_Y + 10, PROBE_X, PROBE_X + 120)
        inside = row_mean(px, w, PANEL_Y + 20, PROBE_X, PROBE_X + 120)
        c.add("the dock is where a dock of forty four pixels would be",
              colour_gap(above, inside) > 60, shot)

        # --- a key written by hand moves the screen -----------------------
        #
        # From the console, which is the point: the file is the interface,
        # and the settings window is one program that writes it.
        vm.run("write /zelr.cfg dock_h %d" % TALL, timeout=20)
        _, _, _, shot, taller = mon.wait_screen(
            "set-taller",
            lambda w, h, px: colour_gap(row_mean(px, w, TALL_Y + 10,
                                                 PROBE_X, PROBE_X + 120),
                                        row_mean(px, w, PANEL_Y + 20,
                                                 PROBE_X, PROBE_X + 120)) < 24,
            timeout=20)
        c.add("a dock height written from the console reaches the dock",
              taller, shot)

        # And back, so nothing below has to know about it.
        vm.run("write /zelr.cfg dock_h %d" % DOCK_H, timeout=20)
        mon.wait_screen(
            "set-back",
            lambda w, h, px: colour_gap(row_mean(px, w, TALL_Y + 10,
                                                 PROBE_X, PROBE_X + 120),
                                        row_mean(px, w, PANEL_Y + 20,
                                                 PROBE_X, PROBE_X + 120)) > 60,
            timeout=20)

        # --- and a switch in the window writes the same keys --------------
        #
        # The terminal the desktop opens with goes first. Where a window
        # lands is where the cascade has got to, so Settings is only at the
        # place worked out above when it is the first window of the
        # session: with the terminal still up it opened eighty pixels along
        # and the clicks below went to the page next to the one meant.
        mon.move_to(950, 200)
        mon.send("mouse_button 2", settle=0.4)
        mon.send("mouse_button 0", settle=1.2)
        mon.click(CTX_X + 60, CTX_Y + CTX_PAD + CTX_CLOSE * CTX_ITEM
                  + CTX_ITEM // 2)
        time.sleep(2.5)

        # Opened from the launcher, reached from the name at the left of the
        # dock. It used to be reached from a field down the middle of the
        # bar; that field is a find button in the tray now and looks for
        # words on the screen rather than for programs, so the launcher has
        # one way in and this is it. What is typed still narrows the list,
        # which is the part this is leaning on.
        mon.click(DOCK_SIDE + 16 + 38, PANEL_Y + DOCK_H // 2)
        time.sleep(1.5)
        for ch in "sett":
            mon.send("sendkey %s" % ch, settle=0.12)
        mon.send("sendkey ret", settle=0.5)

        _, _, _, shot, opened = mon.wait_screen(
            "set-window",
            lambda w, h, px: count_near(px, w,
                                        (IN_X, IN_Y, IN_X + SIDEBAR_W,
                                         IN_Y + 400),
                                        (0xFA, 0xFA, 0xFC), 8) > 20000,
            timeout=45)
        c.add("the settings window opens from a name typed into the launcher",
              opened, shot)

        mon.click(*sidebar("Everything"))
        time.sleep(1.2)

        # The badge on the dock is the fifth row: dock_h, dock_gap,
        # dock_side, dock_radius, then dock_brand. Worked out from the
        # kernel's own order rather than counted off the screen.
        order = [r[0] for r in rows]
        at = order.index("dock_brand")

        w, h, px, _, _ = mon.wait_screen("set-badge", lambda w, h, px: True)
        before = count_near(px, w, BADGE, (0x6E, 0x8A, 0xE8), 60)

        mon.click(*every_toggle(at))
        _, _, _, shot, gone = mon.wait_screen(
            "set-nobadge",
            lambda w, h, px: count_near(px, w, BADGE,
                                        (0x6E, 0x8A, 0xE8), 60) < 20,
            timeout=25)
        c.add("a switch in the window takes the name off the dock",
              gone and before >= 20, shot)

        # Back on, so the switch is a switch rather than a way out.
        mon.click(*every_toggle(at))
        _, _, _, shot, backon = mon.wait_screen(
            "set-badge-back",
            lambda w, h, px: count_near(px, w, BADGE,
                                        (0x6E, 0x8A, 0xE8), 60) >= 20,
            timeout=25)
        c.add("and pressing it again puts it back", backon, shot)

        # --- and the file it wrote says so, in the kernel's own words -----
        #
        # Read from the console, which means leaving the desktop first. A
        # command typed while the desktop is up still runs -- the dock
        # above grew when one was -- but nothing it prints comes back,
        # because the shell is not the thing reading the keyboard any more.
        # So this is the last check: escape hands the machine back.
        mon.send("sendkey esc", settle=1.0)
        time.sleep(2.0)
        vm.wait_prompt(timeout=30)

        cfg = vm.run("cat /zelr.cfg", timeout=20)
        wrote = ("dock_brand 1" in cfg and "dock_h 44" in cfg
                 and "wallpaper 11" in cfg and "anim_ms 120" in cfg)
        c.add("and the file it wrote is the one the kernel reads", wrote)
        if not wrote:
            print("--- /zelr.cfg as the machine has it ---")
            print(cfg)
            print("---------------------------------------")

    finally:
        vm.stop()

    # report() hands back an exit code -- nought for a clean run -- rather
    # than a boolean. Read as one, a passing run exits 1, and the gate
    # printed "all 10 checks passed" beside the word FAIL.
    code = c.report(keep=keep)
    if not keep:
        try:
            os.remove(DISK)
        except OSError:
            pass
    sys.exit(code)


if __name__ == "__main__":
    main()
