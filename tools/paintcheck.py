"""Paint keeps its pictures: a PNG opened, drawn on, saved, undone.

A picture made here with zlib (100 by 60, blue on the left and yellow on
the right) on a fresh volume. Paint says on the console what it did
("paint: opened PATH, 100x60", "paint: saved PATH, WxH"), and what it saved
is read off the disk image (tools/readfat.py) and decoded here, CRCs and
all, by printcheck's reader.

  - started on the picture, Paint shows it on its paper;
  - a stroke drawn under it and ctrl+S: the file is the paper, the picture
    in its top left as it was, and the stroke in Paint's colour;
  - ctrl+Z takes the stroke back, and saved again the file has none of it;
  - started on no file, ctrl+S writes /home/picture.png.

  python tools/paintcheck.py [--keep]
"""
import os
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402
from printcheck import png_pixels                            # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "paintcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "paintcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}
BLUE, YELLOW = (0x20, 0x50, 0xC0), (0xE0, 0xC0, 0x20)
PAPER = (0xF2, 0xF5, 0xF7)                 # paint.c PAPER
STROKE = (0xC7, 0x4A, 0x3C)                # paint.c PALETTE[2], the colour it starts with
PW, PH = 100, 60


def picture():
    rows = b"".join(b"\0" + bytes(BLUE) * (PW // 2) + bytes(YELLOW) * (PW - PW // 2) for _ in range(PH))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", PW, PH, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def box_of(w, h, px, rgb):
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            i = row + x * 3
            if px[i] == rgb[0] and px[i + 1] == rgb[1] and px[i + 2] == rgb[2]:
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    return None if x1 < 0 else (x0, y0, x1, y1)


def read(path):
    try:
        return Fat16(IMAGE).read_file(path)
    except Exception:
        return None


def pixel(png, x, y):
    w, h, rgb, good = png
    i = (y * w + x) * 3
    return tuple(rgb[i:i + 3])


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("paint keeps its pictures")
    os.makedirs(WORK, exist_ok=True)
    src = os.path.join(WORK, "start.png")
    with open(src, "wb") as f:
        f.write(picture())
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", src + ":home/pic/start.png"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a picture on it", r.returncode == 0)
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
            "paint-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

        def line_since(mark, prefix):
            out = vm.serial()[mark:]
            at = out.rfind(prefix)
            return "" if at < 0 else out[at:].split("\n")[0].strip()

        mark = len(vm.serial())
        for ch in "/bin/paint /home/pic/start.png\n":
            key(NAMED.get(ch, ch), settle=0.06)
        opened = said_since(mark, "paint: opened /home/pic/start.png, 100x60", 30)
        _, _, _, shot, seen = mon.wait_screen(
            "paint-open", lambda w, h, px: count_in(px, w, SCREEN, BLUE) >= (PW // 2) * PH - 50, timeout=20)
        c.add("started on a PNG, Paint opens it on its paper", opened and seen, shot)

        w, h, px, _ = mon.screen("paint-find")
        blue = box_of(w, h, px, BLUE)
        if not blue:
            return c.report(keep=keep)
        ox, oy = blue[0], blue[1]                  # the paper's top left on the screen
        row = PH + 40
        mark = len(vm.serial())
        mon.drag((ox + 10, oy + row), (ox + 130, oy + row))
        time.sleep(0.5)
        key("ctrl-s")
        saved = said_since(mark, "paint: saved /home/pic/start.png, ")
        size = line_since(mark, "paint: saved /home/pic/start.png, ").split(", ")[-1]

        def saved_png():
            data = read("/home/pic/start.png")
            return png_pixels(data) if data and data != picture() else None

        got = None
        end = time.time() + 30
        while saved and got is None and time.time() < end:
            got = saved_png()
            time.sleep(0.5)
        ok_file = bool(got) and got[3] and "%dx%d" % (got[0], got[1]) == size
        c.add("ctrl+S writes a PNG, every chunk's CRC good, the size said (%s)" % size, ok_file)
        if got:
            whole = all(pixel(got, x, y) == (BLUE if x < PW // 2 else YELLOW)
                        for y in range(0, PH, 7) for x in range(0, PW, 7))
            stroke = sum(pixel(got, x, row) == STROKE for x in range(15, 125, 5))
            c.add("the picture is in its top left as it was", whole)
            c.add("the stroke is in it, in Paint's colour (%d of 22 along it)" % stroke, stroke >= 20)
            c.add("and the rest is paper", pixel(got, got[0] - 20, got[1] - 20) == PAPER and pixel(got, 300, 20) == PAPER)

        mark = len(vm.serial())
        key("ctrl-z")
        undone = said_since(mark, "paint: undone")
        key("ctrl-s")
        said_since(mark, "paint: saved /home/pic/start.png")
        again = None
        end = time.time() + 30
        while time.time() < end:
            data = read("/home/pic/start.png")
            again = png_pixels(data) if data else None
            if again and pixel(again, 60, row) == PAPER:
                break
            time.sleep(0.5)
        c.add("ctrl+Z takes the stroke back, and saved again it is gone",
              undone and again is not None and pixel(again, 60, row) == PAPER and pixel(again, 10, 10) == BLUE)
        key("alt-q", settle=1.0)
        time.sleep(1.0)

        mark = len(vm.serial())
        for ch in "/bin/paint\n":
            key(NAMED.get(ch, ch), settle=0.06)
        time.sleep(3.0)
        key("ctrl-s")
        c.add("started on no file, ctrl+S writes /home/picture.png",
              said_since(mark, "paint: saved /home/picture.png, ", 30)
              and (lambda d: d is not None and png_pixels(d) is not None)(read("/home/picture.png")))
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    for junk in (src,):
        try:
            os.remove(junk)
        except OSError:
            pass
    try:
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
