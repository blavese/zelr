"""Paint's line, rectangle, ellipse and fill.

Paint is started on a small blue picture, which puts a mark at the top left
of its paper for this to find. The shapes are dragged with the monitor's
mouse, and what Paint saved is read off the disk image and decoded here by
printcheck's reader, so each is checked pixel by pixel in the file, in
paper coordinates.

  - L and a drag that goes out of its way: a straight line between the
    press and the release, and nothing where the pointer went between
    (a brush would have followed it);
  - R and a drag: a rectangle's four sides, and paper inside;
  - F and a click inside it: the inside filled, and nothing outside it;
  - E and a drag: an ellipse in that box, its four sides drawn;
  - and closed all round: filled inside, the fill stays inside it (a gap
    anywhere would let it out over the whole paper).

  python tools/shapecheck.py [--keep]
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
from paintcheck import box_of, PAPER, STROKE                 # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "shapecheck.%d.img" % os.getpid())
SRC = os.path.join(ROOT, "shapecheck.%d.png" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
BLUE = (0x20, 0x50, 0xC0)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}


def marker():
    rows = b"".join(b"\0" + bytes(BLUE) * 20 for _ in range(20))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 20, 20, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("paint's shapes")
    with open(SRC, "wb") as f:
        f.write(marker())
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", SRC + ":home/pic/s.png"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a small mark to draw beside", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    got = None
    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "shape-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

        mark = len(vm.serial())
        for ch in "/bin/paint /home/pic/s.png\n":
            key(NAMED.get(ch, ch), settle=0.06)
        said_since(mark, "paint: opened /home/pic/s.png", 30)
        _, _, _, shot, seen = mon.wait_screen(
            "shape-open", lambda w, h, px: count_in(px, w, SCREEN, BLUE) >= 380, timeout=20)
        w, h, px, _ = mon.screen("shape-find")
        mark_box = box_of(w, h, px, BLUE)
        c.add("Paint opens with the mark on its paper", seen and mark_box is not None, shot)
        if not mark_box:
            return c.report(keep=keep)
        ox, oy = mark_box[0], mark_box[1]

        def drag(a, b):
            mon.drag((ox + a[0], oy + a[1]), (ox + b[0], oy + b[1]))
            time.sleep(0.4)

        def drag_by_way_of(a, via, b):
            """Down at a, over to via, up at b: a shape is from a to b, a
            stroke goes through via."""
            mon.move_to(ox + a[0], oy + a[1])
            mon.send("mouse_button 1", settle=0.2)
            for k in range(1, 6):
                mon.move_to(ox + a[0] + (via[0] - a[0]) * k // 5, oy + a[1] + (via[1] - a[1]) * k // 5)
            for k in range(1, 6):
                mon.move_to(ox + via[0] + (b[0] - via[0]) * k // 5, oy + via[1] + (b[1] - via[1]) * k // 5)
            mon.send("mouse_button 0", settle=0.2)
            time.sleep(0.4)

        mark = len(vm.serial())
        key("l")
        said_since(mark, "paint: a line", 10)
        drag_by_way_of((100, 60), (200, 100), (300, 60))
        key("r")
        said_since(mark, "paint: a rectangle", 10)
        drag((100, 120), (300, 220))
        key("f")
        said_since(mark, "paint: a fill", 10)
        mon.click(ox + 200, oy + 170)
        time.sleep(0.5)
        # The ellipse with the finest brush: a pixel wide, a gap anywhere in
        # its outline lets the fill below out (a wide brush hides gaps).
        key("1")
        key("e")
        said_since(mark, "paint: an ellipse", 10)
        drag((350, 120), (550, 220))
        # Filled inside: an outline with a gap anywhere lets the fill out over
        # the whole paper, which the corners and the rest would show.
        key("f")
        mon.click(ox + 450, oy + 170)
        time.sleep(0.5)
        mark = len(vm.serial())
        key("ctrl-s")
        saved = said_since(mark, "paint: saved /home/pic/s.png")
        got = None
        end = time.time() + 30
        while saved and got is None and time.time() < end:
            try:
                data = Fat16(IMAGE).read_file("/home/pic/s.png")
            except Exception:
                data = None
            got = png_pixels(data) if data else None
            time.sleep(0.5)
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    def at(x, y):
        pw, ph, rgb, _ = got
        i = (y * pw + x) * 3
        return tuple(rgb[i:i + 3])

    def near(x, y, d=3):
        """Drawn within d of the point: a pixel-wide outline lands where the
        drag did, which the monitor's mouse gives to a pixel or two."""
        return any(at(x + dx, y + dy) == STROKE for dx in range(-d, d + 1) for dy in range(-d, d + 1))

    c.add("saved, and read back here", got is not None)
    if got:
        c.add("L and a drag out of its way: a straight line from press to release, nothing where it went between",
              all(at(x, 60) == STROKE for x in range(110, 291, 10)) and at(200, 100) == PAPER and at(150, 80) == PAPER
              and at(250, 80) == PAPER and at(200, 40) == PAPER)
        c.add("R and a drag: a rectangle's four sides",
              at(200, 120) == STROKE and at(200, 220) == STROKE and at(100, 170) == STROKE and at(300, 170) == STROKE)
        c.add("F and a click inside it: the inside filled, and nothing outside it",
              at(200, 170) == STROKE and at(150, 150) == STROKE and at(200, 240) == PAPER and at(60, 170) == PAPER)
        c.add("E and a drag: an ellipse in that box, its four sides drawn",
              near(350, 170) and near(550, 170) and near(450, 120) and near(450, 220))
        c.add("closed all round: filled inside, the fill stays in it (the box's corners and beyond are paper)",
              at(450, 170) == STROKE and at(420, 190) == STROKE and at(356, 126) == PAPER and at(544, 214) == PAPER
              and at(450, 300) == PAPER and at(600, 60) == PAPER)
    for junk in (SRC,) + (() if keep else (IMAGE,)):
        try:
            os.remove(junk)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
