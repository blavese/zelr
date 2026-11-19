"""Pictures zoomed, moved about and turned.

One picture on a fresh volume, 200 by 100, red on the left half and blue on
the right. The viewer says on the console what it shows ("pictures: NAME, W
by H, N%"), and the screen is counted by colour:

  - + doubles it, twice the size each way, four times the pixels;
  - at eight times it is bigger than the window, and the right arrow moves
    over it towards the blue;
  - 0 fits it again;
  - r turns it a quarter to the right: 100 by 200, the red on top.

  python tools/zoomcheck.py [--keep]
"""
import os
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "zoomcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "zoomcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
RED = (0xE0, 0x30, 0x40)
BLUE = (0x30, 0x50, 0xE0)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}


def png(path, w, h, pixel):
    rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def mean_y(px, w, h, colour):
    """The mean row of the pixels of a colour, or -1."""
    total = n = 0
    want = bytes(colour)
    for y in range(0, h, 2):
        row = px[y * w * 3:(y + 1) * w * 3]
        k = row.count(want)        # close enough: whole pixels of it in the row, by bytes
        if k:
            total += y * k
            n += k
    return total / n if n else -1


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("pictures zoomed")
    os.makedirs(WORK, exist_ok=True)
    z = os.path.join(WORK, "z.png")
    png(z, 200, 100, lambda x, y: RED if x < 100 else BLUE)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", z + ":home/pictures/z.png"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a picture, red on the left and blue on the right", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "zoom-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def keys(text):
            for ch in text:
                mon.send("sendkey %s" % NAMED.get(ch, ch), settle=0.06)

        def said_since(mark, text, timeout=30):
            end = time.time() + timeout
            while time.time() < end:
                if text.lower() in vm.serial()[mark:].lower():   # mkfat's names come back lower case
                    return True
                time.sleep(0.2)
            return False

        mark = len(vm.serial())
        keys("/bin/pictures /home/pictures/z.png\n")
        shown = said_since(mark, "pictures: z.png, 200 by 100, 100%", 40)
        _, _, _, shot, drawn = mon.wait_screen(
            "zoom-1", lambda w, h, px: abs(count_in(px, w, SCREEN, RED) - 10000) < 300
            and abs(count_in(px, w, SCREEN, BLUE) - 10000) < 300, timeout=20)
        c.add("it opens at its own size", shown and drawn, shot)

        mark = len(vm.serial())
        mon.send("sendkey equal", settle=0.4)
        shown = said_since(mark, "pictures: z.png, 200 by 100, 200%")
        _, _, _, shot, drawn = mon.wait_screen(
            "zoom-2", lambda w, h, px: abs(count_in(px, w, SCREEN, RED) - 40000) < 800
            and abs(count_in(px, w, SCREEN, BLUE) - 40000) < 800, timeout=20)
        c.add("+ doubles it: four times the pixels of each colour", shown and drawn, shot)

        mark = len(vm.serial())
        mon.send("sendkey equal", settle=0.4)
        mon.send("sendkey equal", settle=0.4)
        shown = said_since(mark, "pictures: z.png, 200 by 100, 800%")
        w, h, px, shot, _ = mon.wait_screen("zoom-8", lambda w, h, px: True)
        red8, blue8 = count_in(px, w, SCREEN, RED), count_in(px, w, SCREEN, BLUE)
        c.add("at eight times it is bigger than the window, both halves showing (%d red, %d blue)" % (red8, blue8),
              shown and red8 > 20000 and blue8 > 20000)

        mark = len(vm.serial())
        mon.send("sendkey right", settle=0.4)
        _, _, _, shot, moved = mon.wait_screen(
            "zoom-right", lambda w, h, px: count_in(px, w, SCREEN, BLUE) > blue8 + 20000
            and count_in(px, w, SCREEN, RED) < red8 - 20000, timeout=20)
        c.add("the right arrow moves over it, towards the blue", moved, shot)

        mark = len(vm.serial())
        mon.send("sendkey 0", settle=0.4)
        shown = said_since(mark, "pictures: z.png, 200 by 100, 100%")
        _, _, _, shot, drawn = mon.wait_screen(
            "zoom-fit", lambda w, h, px: abs(count_in(px, w, SCREEN, RED) - 10000) < 300, timeout=20)
        c.add("0 fits it again", shown and drawn, shot)

        mark = len(vm.serial())
        mon.send("sendkey r", settle=0.4)
        shown = said_since(mark, "pictures: z.png, 100 by 200, 100%")
        w, h, px, shot, drawn = mon.wait_screen(
            "zoom-turned", lambda w, h, px: abs(count_in(px, w, SCREEN, RED) - 10000) < 300
            and abs(count_in(px, w, SCREEN, BLUE) - 10000) < 300, timeout=20)
        ry, by = mean_y(px, w, h, RED), mean_y(px, w, h, BLUE)
        c.add("r turns it a quarter to the right: 100 by 200, the red on top (rows %d and %d)" % (ry, by),
              shown and drawn and 0 <= ry < by - 60)
        mon.send("sendkey alt-q", settle=1.0)
    finally:
        vm.stop()

    try:
        os.remove(z)
        os.rmdir(WORK)
    except OSError:
        pass
    if not keep:
        try:
            os.remove(IMAGE)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
