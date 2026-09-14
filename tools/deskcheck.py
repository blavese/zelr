"""Drives the desktop and checks the windows actually moved.

Minimising, maximising, snapping and resizing all end in a window covering a
different part of the screen, and the terminal paints its page one flat
colour. So counting that colour, and where it is, says exactly what the
window manager did without needing to read anything on screen.

The terminal opens as the first window at a known place, which is what makes
the button coordinates below predictable:

    x=40 y=36, content 760x480, so the outer frame is 762x505
    the three title buttons sit 26, 46 and 66 pixels in from the right edge

  python tools/deskcheck.py [--keep]
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
DISK = os.path.join(ROOT, "deskcheck.img")
MONITOR_PORT = 55735

QEMU = os.environ.get("QEMU") or "C:/Program Files/qemu/qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    from shutil import which
    QEMU = which("qemu-system-x86_64") or QEMU

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shotcheck import Monitor, count_in      # noqa: E402

PAGE = (0x10, 0x14, 0x1A)          # the terminal's default background
SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H = 34

# The terminal as it opens.
WIN_X, WIN_Y, WIN_CW, WIN_CH = 40, 36, 760, 480
OUTER_W = WIN_CW + 2
OUTER_H = WIN_CH + 24 + 1

BTN_Y = WIN_Y + 5 + 7
BTN_CLOSE = (WIN_X + OUTER_W - 26 + 7, BTN_Y)
BTN_MAX = (WIN_X + OUTER_W - 46 + 7, BTN_Y)
BTN_MIN = (WIN_X + OUTER_W - 66 + 7, BTN_Y)
GRIP = (WIN_X + OUTER_W - 7, WIN_Y + OUTER_H - 7)

# The same buttons once the window has been maximised, when its frame is at
# 0,0 and as wide as the screen.
BTN_MAX_WHEN_MAXIMISED = (SCREEN_W - 46 + 7, 5 + 7)
TASKBAR_CHIP = (150, SCREEN_H - TASKBAR_H + 17)

WHOLE = (0, 0, SCREEN_W, SCREEN_H)
LEFT_HALF = (0, 0, SCREEN_W // 2, SCREEN_H - TASKBAR_H)
RIGHT_HALF = (SCREEN_W // 2, 0, SCREEN_W, SCREEN_H - TASKBAR_H)


def page_pixels(mon, name, shots, rect=WHOLE):
    w, h, px, ppm = mon.screen(name)
    shots.append(ppm)
    return count_in(px, w, rect, PAGE)


# What the monitor calls the keys that are not letters.
NAMED = {" ": "spc", "\n": "ret", "/": "slash", ".": "dot"}


def typed(mon, text):
    for ch in text:
        mon.send("sendkey %s" % NAMED.get(ch, ch), settle=0.06)


def wallpaper_shot(mon, name, shots):
    """The bare desktop, as raw bytes, so two of them can be compared without
    caring what is actually drawn on it."""
    w, h, px, ppm = mon.screen(name)
    shots.append(ppm)
    out = bytearray()
    for y in range(0, SCREEN_H - TASKBAR_H, 3):
        row = y * w * 3
        out += px[row:row + SCREEN_W * 3]
    return bytes(out)


def alt(mon, key):
    """A chord. The monitor spells these with a dash."""
    mon.send("sendkey alt-%s" % key, settle=0.4)


def drag(mon, frm, to):
    """Press, move, release, with the move split so the manager sees the
    pointer travel rather than teleport."""
    mon.move_to(*frm)
    mon.send("mouse_button 1", settle=0.3)
    steps = 6
    for i in range(1, steps + 1):
        x = frm[0] + (to[0] - frm[0]) * i // steps
        y = frm[1] + (to[1] - frm[1]) * i // steps
        mon.move_to(x, y)
    mon.send("mouse_button 0", settle=0.6)


def main():
    keep = "--keep" in sys.argv
    # Built here when this is run on its own, and not when the gate runs
    # it. The gate builds once and then starts several harnesses at the
    # same time; a second build rewrites build/nyx.bin and build/nyx.elf
    # underneath whichever machine is reading them, which on Windows is a
    # permission error rather than a torn file.
    if os.environ.get("NYX_PREBUILT") != "1":
        subprocess.run(["bash", "build.sh"], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)

    if os.path.exists(DISK):
        os.remove(DISK)
    with open(DISK, "wb") as f:
        f.truncate(32 * 1024 * 1024)

    proc = subprocess.Popen(
        [QEMU, "-kernel", os.path.join(BUILD, "nyx.bin"), "-m", "64",
         "-no-reboot", "-display", "none", "-serial", "stdio",
         "-drive", "file=%s,format=raw,if=ide,index=0" % DISK,
         "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONITOR_PORT],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT)

    shots = []
    checks = []

    try:
        time.sleep(4.5)
        for ch in "desktop\n":
            proc.stdin.write(ch.encode())
            proc.stdin.flush()
            time.sleep(0.05)
        time.sleep(5.0)

        mon = Monitor(MONITOR_PORT)
        mon.move_to(500, 400)

        opened = page_pixels(mon, "desk-open", shots)
        checks.append(("the terminal opens at the size it asked for",
                       300000 < opened < 380000))

        # --- minimise -----------------------------------------------------
        mon.click(*BTN_MIN)
        time.sleep(1.5)
        away = page_pixels(mon, "desk-minimised", shots)
        checks.append(("the minimise button takes the window off screen",
                       away < 1000))

        # --- and back, from the taskbar -----------------------------------
        mon.click(*TASKBAR_CHIP)
        time.sleep(1.5)
        back = page_pixels(mon, "desk-restored", shots)
        checks.append(("clicking it in the taskbar brings it back",
                       abs(back - opened) < 20000))

        # --- maximise -----------------------------------------------------
        mon.click(*BTN_MAX)
        time.sleep(2.0)
        big = page_pixels(mon, "desk-maximised", shots)
        checks.append(("the maximise button fills the screen above the taskbar",
                       big > 650000))

        # A maximised window really is a bigger surface, not the same one
        # stretched: the terminal was told to redraw at the new size and its
        # page now reaches the bottom of the work area.
        w, h, px, ppm = mon.screen("desk-maximised-b")
        shots.append(ppm)
        bottom_strip = count_in(px, w, (100, SCREEN_H - TASKBAR_H - 40,
                                        900, SCREEN_H - TASKBAR_H - 10), PAGE)
        checks.append(("and the program redrew into the space it was given",
                       bottom_strip > 20000))

        # --- restore ------------------------------------------------------
        mon.click(*BTN_MAX_WHEN_MAXIMISED)
        time.sleep(2.0)
        small = page_pixels(mon, "desk-unmaximised", shots)
        checks.append(("pressing it again puts the window back",
                       abs(small - opened) < 20000))

        # --- resizing by the corner ---------------------------------------
        #
        # Done here, while the window is known to be back at the size and
        # place it opened at, so the grip is where it was.
        drag(mon, GRIP, (GRIP[0] - 200, GRIP[1] - 150))
        time.sleep(2.0)
        after = page_pixels(mon, "desk-resized", shots)
        checks.append(("dragging the corner makes the window smaller",
                       after < small - 100000))

        # --- snapping with the keyboard -----------------------------------
        alt(mon, "left")
        time.sleep(2.0)
        w, h, px, ppm = mon.screen("desk-snap-left")
        shots.append(ppm)
        on_left = count_in(px, w, LEFT_HALF, PAGE)
        on_right = count_in(px, w, RIGHT_HALF, PAGE)
        checks.append(("alt and left snaps a window to that half",
                       on_left > 300000 and on_right < 1000))

        alt(mon, "right")
        time.sleep(2.0)
        w, h, px, ppm = mon.screen("desk-snap-right")
        shots.append(ppm)
        on_left = count_in(px, w, LEFT_HALF, PAGE)
        on_right = count_in(px, w, RIGHT_HALF, PAGE)
        checks.append(("and alt and right to the other",
                       on_right > 300000 and on_left < 1000))

        # --- the wallpapers -----------------------------------------------
        #
        # Set through the config file, from the terminal, which is the same
        # path the settings window uses: a ring 3 program writes a file and
        # the window manager notices. Everything is put away first so what
        # is measured is only the desktop.
        # The terminal is on screen already at this point, so clicking its
        # taskbar chip would put it away rather than bring it back.
        time.sleep(0.5)
        typed(mon, "write /nyx.cfg wallpaper 4\n")     # stars
        time.sleep(2.5)
        alt(mon, "d")
        time.sleep(1.5)
        stars_a = wallpaper_shot(mon, "desk-stars-a", shots)

        time.sleep(1.5)
        stars_b = wallpaper_shot(mon, "desk-stars-b", shots)
        checks.append(("a wallpaper written from ring 3 reaches the desktop",
                       len(stars_a) > 0))
        checks.append(("and one that drifts is not the same twice",
                       stars_a != stars_b))

        mon.click(*TASKBAR_CHIP)
        time.sleep(1.5)
        typed(mon, "write /nyx.cfg wallpaper 6\n")     # weave
        time.sleep(2.5)
        alt(mon, "d")
        time.sleep(1.5)
        weave = wallpaper_shot(mon, "desk-weave", shots)
        checks.append(("a different wallpaper draws differently",
                       weave != stars_a and weave != stars_b))

        # --- alt+tab ------------------------------------------------------
        #
        # Open a second window, then check the front one changes. Paint
        # covers its canvas in its own colour, so which is in front is
        # visible in how much of the terminal is left showing.
        mon.click(40, SCREEN_H - TASKBAR_H + 17)      # the launcher badge
        time.sleep(1.0)
        mon.click(60, 645)                            # Paint, second entry
        time.sleep(4.0)
        w, h, px, ppm = mon.screen("desk-two")
        shots.append(ppm)
        two_up = count_in(px, w, WHOLE, PAGE)

        alt(mon, "tab")
        time.sleep(1.5)
        w, h, px, ppm = mon.screen("desk-cycled")
        shots.append(ppm)
        cycled = count_in(px, w, WHOLE, PAGE)
        checks.append(("alt and tab brings the window behind to the front",
                       cycled != two_up))

        # --- show the desktop ---------------------------------------------
        alt(mon, "d")
        time.sleep(1.5)
        cleared = page_pixels(mon, "desk-cleared", shots)
        checks.append(("alt and d puts everything away at once", cleared < 1000))

        mon.close()

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        if not keep and os.path.exists(DISK):
            os.remove(DISK)

    print("=== desktop test ===")
    failed = 0
    for name, passed in checks:
        print("  %s  %s" % ("PASS" if passed else "FAIL", name))
        if not passed:
            failed += 1
    print()
    if failed:
        print("desktop test: %d failed" % failed)
        print("screenshots: %s" % ", ".join(shots))
    else:
        print("desktop test: all %d checks passed" % len(checks))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
