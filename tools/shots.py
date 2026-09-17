"""Takes the pictures the readme uses.

Screenshots in a readme go stale silently. The one that was there had been
taken before the desktop had a design, a taskbar that floats, a typeface with
weights, or any of the windows it now shows, and nothing anywhere would ever
have said so.

So they are made the same way the tests are: boot the thing, drive it, and
photograph what it actually does. Run this after any change to how the desktop
looks and commit what comes out.

  python tools/shots.py

The PNG writer is here because the pictures come out of QEMU as P6 PPM and
nothing in this project is allowed to depend on an image library. zlib is in
the Python standard library and does the only hard part.
"""
import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, build_once, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "shots.%d.img" % os.getpid())
OUT = os.path.join(ROOT, "docs")

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H = 34
TASKBAR_GAP = 10
MENU_ITEM = 30
MENU_ENTRIES = 9
MENU_TOP = (SCREEN_H - TASKBAR_H - TASKBAR_GAP
            - (MENU_ENTRIES * MENU_ITEM + 12) - 8)

LAUNCHER = (40, SCREEN_H - TASKBAR_H - TASKBAR_GAP + 17)
PARK = (1010, SCREEN_H - 10)

# The apps kept on the panel start past the badge, 30 pixels apart, with the
# terminal first. Its icon is what brings the terminal back.
TERMINAL_ICON = (TASKBAR_GAP + 8 + 76 + 12 + 11,
                 SCREEN_H - TASKBAR_H - TASKBAR_GAP + 15)

# Somewhere the pointer is not asking for the panel. Anywhere along the
# bottom of the screen brings it back out over whatever is maximised, which
# is the opposite of what the picture is of.
HIGH = (1010, 300)

# The terminal as it opens, and the buttons on it.
WIN_X, WIN_Y, WIN_CW = 40, 36, 760
OUTER_W = WIN_CW + 2
BTN_CLOSE = (WIN_X + OUTER_W - 26 + 7, WIN_Y + 5 + 7)


def write_png(path, w, h, pixels):
    """A PPM's worth of pixels, as a PNG.

    Colour type 2 is 8 bit RGB with no palette and no alpha, which is what
    comes out of the framebuffer. Every row carries a filter byte of zero,
    meaning the row is stored as it is: filtering would compress better but
    it would also be the only clever thing in this file.
    """
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += pixels[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")

    with open(path, "wb") as f:
        f.write(png)
    return len(png)


# What the launcher lists, in the order it lists it.
TERMINAL, FILES, NOTES, PAINT, SETTINGS = 0, 1, 2, 3, 4


def menu_item(n):
    return (90, MENU_TOP + MENU_ITEM * n + 15)


def stroke(mon, x0, y0, steps):
    """A drag, for drawing in Paint.

    The button is held down across a run of relative moves rather than
    clicked at each point, because a press and a release inside one pass of
    the window manager's loop is a click it never sees at all.
    """
    mon.move_to(x0, y0)
    mon.send("mouse_button 1", settle=0.3)
    for dx, dy in steps:
        mon.send("mouse_move %d %d" % (dx, dy), settle=0.06)
    mon.send("mouse_button 0", settle=0.4)


def shoot(mon, name, settle=0.6, park=PARK):
    """One picture, parked so the pointer is never mid-screen in it."""
    mon.move_to(*park)
    time.sleep(settle)
    w, h, px, ppm = mon.screen("shot-" + name)

    path = os.path.join(OUT, name + ".png")
    size = write_png(path, w, h, px)
    try:
        os.remove(ppm)
    except OSError:
        pass
    print("  %-14s %dx%d  %d KiB" % (name + ".png", w, h, size // 1024))


def main():
    build_once()
    os.makedirs(OUT, exist_ok=True)

    vm = Guest(DISK, memory=64)
    try:
        vm.wait_boot()
        mon = vm.monitor()

        # --- the console, before anything graphical happens ---------------
        vm.type("uname\n")
        time.sleep(1.0)
        vm.type("disk\n")
        time.sleep(1.0)
        shoot(mon, "boot")

        # --- the desktop, with the terminal it opens with -----------------
        vm.type("desktop\n")
        time.sleep(4.0)
        vm.type("help\n")
        time.sleep(1.5)
        shoot(mon, "desktop")

        # --- the launcher open over it ------------------------------------
        mon.click(*LAUNCHER)
        time.sleep(1.2)
        shoot(mon, "launcher")

        # --- two windows, and which one has focus -------------------------
        mon.click(*menu_item(FILES))
        time.sleep(2.5)
        shoot(mon, "files")

        # --- paint, with something in it ----------------------------------
        mon.click(*LAUNCHER)
        time.sleep(1.0)
        mon.click(*menu_item(PAINT))
        time.sleep(2.5)
        stroke(mon, 300, 300, [(14, 10)] * 14)
        stroke(mon, 300, 440, [(14, -10)] * 14)
        stroke(mon, 520, 280, [(0, 12)] * 13)
        shoot(mon, "paint")

        # --- and settings, where the colours come from --------------------
        mon.click(*LAUNCHER)
        time.sleep(1.0)
        mon.click(*menu_item(SETTINGS))
        time.sleep(2.5)
        shoot(mon, "settings")

        # --- a wallpaper that moves, with nothing in front of it ----------
        #
        # The terminal is brought to the front first. Typing goes to whatever
        # window has focus, and what had it was the settings window, which
        # ignored the whole line: the picture came out showing the wallpaper
        # that was already on.
        mon.click(*TERMINAL_ICON)
        time.sleep(1.5)
        vm.type("write /zelr.cfg wallpaper 7\n")
        time.sleep(1.5)
        mon.send("sendkey alt-d", settle=1.8)
        shoot(mon, "wallpaper")

        # --- and the panel out of the way of a maximised window -----------
        mon.click(*TERMINAL_ICON)
        time.sleep(1.5)
        mon.send("sendkey alt-up", settle=1.8)
        shoot(mon, "maximised", park=HIGH)

    finally:
        vm.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
