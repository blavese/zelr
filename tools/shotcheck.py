"""Boots nyx headless, uses the desktop, and looks at the screen.

The self test can prove the window server hands out a surface, but not that
anything reaches the display or that input comes back. This drives QEMU's
monitor to move the real mouse and to grab the real screen.

Everything it checks crosses a boundary the other tests cannot:

  - the terminal is a ring 3 program, so its window appearing at all means
    the window server, the compositor and the event queue all worked
  - the launcher opens Settings, another ring 3 program
  - clicking an accent in Settings writes a file, and the kernel's window
    manager re-reads it, so the desktop behind changes colour

  python tools/shotcheck.py [--keep]
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
DISK = os.path.join(ROOT, "shotcheck.img")
MONITOR_PORT = 55731

QEMU = os.environ.get("QEMU") or "C:/Program Files/qemu/qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    from shutil import which
    QEMU = which("qemu-system-x86_64") or QEMU

TEAL = (0x2C, 0xC7, 0xA0)
INDIGO = (0x6E, 0x8A, 0xE8)

# Where things are on a 1024x768 screen with the default layout.
BADGE = (40, 745)             # the taskbar launcher
# The launcher's entries, in the order wm.c lists them. The menu grows
# upward from the taskbar, so adding a program moves everything above it and
# a remembered y is wrong from then on.
MENU_ENTRIES = ["Terminal", "Files", "Notes", "Paint", "Settings",
                "System info", "Close all", "Leave desktop"]
MENU_ITEM_H = 30
MENU_BOTTOM = 734             # the menu's lower edge, just above the taskbar
# Settings opens as the second window, so its content starts at 89,98 and the
# second accent swatch sits 70,74 into that.


class Monitor:
    """QEMU's text monitor over TCP. Used to move the mouse and grab the
    screen, neither of which the guest can be asked to do for us."""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        time.sleep(0.4)
        self.s.recv(65536)

    def send(self, command, settle=0.3):
        self.s.sendall((command + "\n").encode())
        time.sleep(settle)
        try:
            return self.s.recv(65536).decode("utf-8", "replace")
        except socket.timeout:
            return ""

    def move_to(self, x, y):
        """The PS/2 pointer is relative and one large jump gets clamped, so
        walk it into the corner and step out from there.

        The step out has to be split as well. Measured: a single move to
        x=985 left the pointer at 635, which is how a click aimed at a
        button on the right of the screen quietly lands in the middle of a
        title bar instead. Nothing reports this; the click simply does
        something else."""
        for _ in range(12):
            self.send("mouse_move -200 -200", settle=0.05)

        STEP = 100
        at_x = at_y = 0
        while at_x < x or at_y < y:
            dx = min(STEP, x - at_x)
            dy = min(STEP, y - at_y)
            self.send("mouse_move %d %d" % (dx, dy), settle=0.05)
            at_x += dx
            at_y += dy
        time.sleep(0.3)

    def click(self, x, y):
        self.move_to(x, y)
        self.send("mouse_button 1", settle=0.25)
        self.send("mouse_button 0", settle=0.6)

    def screen(self, name):
        ppm = os.path.join(BUILD, name + ".ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        self.send("screendump %s" % ppm.replace("\\", "/"), settle=2.5)
        if not os.path.exists(ppm):
            raise RuntimeError("no screenshot from the monitor")
        return read_ppm(ppm) + (ppm,)

    def close(self):
        self.s.close()


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 ppm")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h, _maxval = fields
    return w, h, data[pos:pos + w * h * 3]


def tally(px):
    counts = {}
    for i in range(0, len(px), 3):
        counts[px[i:i + 3]] = counts.get(px[i:i + 3], 0) + 1
    return counts


def centre_of(px, w, h, rgb, min_pixels=200):
    """Where a block of one colour is, as (x, y) of its middle.

    Clicking a remembered coordinate is how this harness kept breaking. A
    swatch moves because a window gained a sidebar, or a menu entry moves
    because two programs were added above it, and the click lands on
    whatever is there now: the check fails, and it reads as the feature
    being broken rather than the test being out of date. So the thing is
    found in the picture instead. Returns None when it is not on screen,
    which is a real answer and not a coordinate to click anyway.
    """
    want = bytes(rgb)
    xs, ys, n = 0, 0, 0
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            i = row + x * 3
            if px[i:i + 3] == want:
                xs += x
                ys += y
                n += 1
    if n < min_pixels:
        return None
    return (xs // n, ys // n)


def count_in(px, w, rect, rgb):
    left, top, right, bottom = rect
    want = bytes(rgb)
    n = 0
    for y in range(top, bottom):
        row = y * w * 3
        for x in range(left, right):
            i = row + x * 3
            if px[i:i + 3] == want:
                n += 1
    return n


def main():
    subprocess.run(["bash", "build.sh"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)

    # A fresh disk every run, so a config left by a previous one cannot make
    # the accent test pass before it has done anything.
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
        time.sleep(4.5)                        # let it boot
        for ch in "desktop\n":                 # typed, not pasted: see shell_test.sh
            proc.stdin.write(ch.encode())
            proc.stdin.flush()
            time.sleep(0.05)
        time.sleep(5.0)                        # the terminal starts and draws

        mon = Monitor(MONITOR_PORT)
        mon.move_to(*BADGE)

        w, h, px, ppm = mon.screen("desktop")
        shots.append(ppm)
        counts = tally(px)
        checks += [
            ("the screen is the mode that was asked for", (w, h) == (1024, 768)),
            ("a ring 3 terminal drew its window", counts.get(bytes(TEAL), 0) > 3000),
            ("the terminal has a dark page to type on",
             count_in(px, w, (120, 120, 700, 480), (0x10, 0x14, 0x1A)) > 100000),
        ]

        mon.click(*BADGE)
        time.sleep(0.6)
        w, h, px, ppm = mon.screen("launcher")
        shots.append(ppm)
        # the menu is a panel one shade lighter than the window chrome,
        # sitting over the wallpaper in the lower left
        menu_pixels = count_in(px, w, (10, 540, 210, 730), (0x1F, 0x27, 0x2F))
        checks.append(("the launcher menu opens", menu_pixels > 8000))

        # Counted from the bottom of the menu rather than remembered.
        idx = MENU_ENTRIES.index("Settings")
        from_bottom = len(MENU_ENTRIES) - 1 - idx
        mon.click(60, MENU_BOTTOM - 6 - from_bottom * MENU_ITEM_H - MENU_ITEM_H // 2)
        time.sleep(3.0)
        w, h, px, ppm = mon.screen("settings")
        shots.append(ppm)
        counts = tally(px)
        checks += [
            ("it launches settings, another ring 3 program",
             counts.get(bytes(INDIGO), 0) > 500),
            ("whose accent swatches are all on screen",
             all(counts.get(bytes(c), 0) > 200 for c in
                 [TEAL, INDIGO, (0xE0, 0xA0, 0x3C), (0xE0, 0x6A, 0x8C),
                  (0x8A, 0x9B, 0xB0), (0x9A, 0xD1, 0x4A)])),
        ]
        teal_before = counts.get(bytes(TEAL), 0)
        indigo_before = counts.get(bytes(INDIGO), 0)

        spot = centre_of(px, w, h, INDIGO)
        checks.append(("the indigo swatch is findable on screen", spot is not None))
        if spot is None:
            raise RuntimeError("no indigo swatch on screen to click")
        mon.click(*spot)
        time.sleep(2.5)
        w, h, px, ppm = mon.screen("recoloured")
        shots.append(ppm)
        counts = tally(px)
        # The two accents should have traded places: what was teal chrome is
        # now indigo chrome, and only the one swatch of each remains.
        checks += [
            ("choosing an accent repaints the window manager",
             counts.get(bytes(INDIGO), 0) > indigo_before * 4),
            ("and the old accent is gone from the chrome",
             counts.get(bytes(TEAL), 0) < teal_before / 4),
        ]

        mon.close()
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        proc.kill()
        proc.wait(timeout=10)

    failed = 0
    print("=== desktop check ===")
    for name, good in checks:
        print("  %s  %s" % ("PASS" if good else "FAIL", name))
        if not good:
            failed += 1

    if "--keep" in sys.argv:
        try:
            from PIL import Image
            for ppm in shots:
                Image.open(ppm).save(ppm[:-4] + ".png")
                print("kept %s" % (ppm[:-4] + ".png"))
        except ImportError:
            print("kept the ppm files; install pillow for png")
    for ppm in shots:
        if os.path.exists(ppm):
            os.remove(ppm)
    if os.path.exists(DISK):
        os.remove(DISK)

    print("\ndesktop check: %s" %
          ("all checks passed" if not failed else "%d failed" % failed))
    return failed


if __name__ == "__main__":
    sys.exit(main())
