"""Pictures' slideshow.

Four small pictures in a folder on a fresh volume; Pictures opened on it
from the terminal. Pictures says on the console each picture it shows, so
the slideshow is followed by those lines and when they came.

  - S starts it, and says so;
  - each picture follows the last in name order, the first again after
    the last, a few seconds apart (three on the machine's clock; between
    two and nine allowed here, as the machine's time runs slow beside other
    work);
  - S again stops it, and nothing changes after;
  - started again, the right arrow stops it as well as moving on.

  python tools/slidecheck.py [--keep]
"""
import os
import re
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "slidecheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "slidecheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
COLOURS = {"a.png": (200, 40, 40), "b.png": (40, 200, 40), "c.png": (40, 40, 200), "d.png": (200, 200, 40)}


def png(w, h, rgb):
    rows = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("pictures' slideshow")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for name, rgb in COLOURS.items():
        p = os.path.join(WORK, name)
        with open(p, "wb") as f:
            f.write(png(40, 30, rgb))
        args.append(p + ":home/p/" + name)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with four pictures", r.returncode == 0)
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
            "slide-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

        def shown_since(mark):
            return re.findall(r"pictures: ([a-d]\.png),", vm.serial()[mark:])

        mark = len(vm.serial())
        vm.type("/bin/pictures /home/p\n")
        c.add("Pictures opens on the first, a.png", said_since(mark, "pictures: a.png, 40 by 30", 30))
        time.sleep(1)

        mark = len(vm.serial())
        key("s")
        c.add("S starts a slideshow", said_since(mark, "pictures: slideshow, every 3 seconds", 10))
        seen, when = [], []
        end = time.time() + 40
        while time.time() < end and len(seen) < 5:
            got = shown_since(mark)
            if len(got) > len(seen):
                for name in got[len(seen):]:
                    seen.append(name)
                    when.append(time.time())
            time.sleep(0.1)
        gaps = [round(b - a, 1) for a, b in zip(when, when[1:])]
        print("      | shown %s, %s seconds apart" % (seen, gaps))
        c.add("it goes b, c, d, then a again after the last, and b", seen == ["b.png", "c.png", "d.png", "a.png", "b.png"])
        c.add("a few seconds apart, not at once (%s)" % gaps, len(gaps) == 4 and all(2.0 <= g <= 9.0 for g in gaps))

        mark = len(vm.serial())
        key("s")
        stopped = said_since(mark, "pictures: slideshow stopped", 10)
        time.sleep(7)
        c.add("S again stops it, and nothing changes after", stopped and not shown_since(mark))

        key("s")
        time.sleep(0.5)
        mark = len(vm.serial())
        key("right")
        stopped = said_since(mark, "pictures: slideshow stopped", 10)
        moved = said_since(mark, "pictures: c.png", 10)
        time.sleep(7)
        c.add("the right arrow stops it as well as moving on, and nothing changes after",
              stopped and moved and shown_since(mark) == ["c.png"])
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    for name in COLOURS:
        try:
            os.remove(os.path.join(WORK, name))
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
