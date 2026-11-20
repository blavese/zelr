"""Paint's words: typed onto the paper in the desktop's own typeface.

Paint is started on shapecheck's small blue picture, which marks the top
left of its paper. Words are typed at clicks, at three of the brush's sizes,
one with a letter taken back, one ended by a click rather than Return, and
one taken back whole with ctrl+Z; what Paint saved is read off the disk and
compared, pixel for pixel, with the same words drawn here from face.h's own
tables, blended at their edges as draw.h blends them.

  - the letters that choose tools (e, l, r, b, 4) are letters in words;
  - Backspace takes a letter back, a click ends one set of words and
    starts the next;
  - the sizes: 15 bold, 20, and 20 at twice its size (40);
  - ctrl+Z takes the last words away whole;
  - the saved picture is exactly the paper with those words on it, and
    nothing else (no mark where the next letter would go).

  python tools/wordscheck.py [--keep]
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402
from printcheck import png_pixels                            # noqa: E402
from paintcheck import box_of, PAPER                         # noqa: E402
from shapecheck import marker, BLUE                          # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "wordscheck.%d.img" % os.getpid())
SRC = os.path.join(ROOT, "wordscheck.%d.png" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret", "!": "shift-1"}


def faces():
    """face.h's faces in face_faces' order: (size, glyphs, coverage)."""
    src = open(os.path.join(ROOT, "userland", "face.h"), encoding="utf-8").read()
    px = {m.group(1): bytes(int(v) for v in m.group(2).split(",") if v.strip())
          for m in re.finditer(r"static const unsigned char (face_px_\w+)\[\d+\] = \{([^}]*)\};", src)}
    gl = {}
    for m in re.finditer(r"static const face_glyph (face_g_\w+)\[95\] = \{(.*?)\n\};", src, re.S):
        rows = re.findall(r"\{ *(-?\d+), *(-?\d+), *(-?\d+), *(-?\d+), *(-?\d+), *(\d+) *\}", m.group(2))
        gl[m.group(1)] = [tuple(int(v) for v in r) for r in rows]
    return [(int(s), gl[g], px[p]) for s, g, p in
            re.findall(r"\{ *(\d+), *\d+, *\d+, *(face_g_\w+), *(face_px_\w+) *\}", src)]


def paint_tables():
    """Paint's palette and which face and size each brush gives its words."""
    src = open(os.path.join(ROOT, "userland", "paint.c"), encoding="utf-8").read()
    pal = src[src.index("PALETTE[16]"):]
    pal = pal[:pal.index("};")]
    palette = [tuple(int(v, 16) for v in m) for m in re.findall(r"RGB\((0x\w+), *(0x\w+), *(0x\w+)\)", pal)]
    face = [int(v) for v in re.search(r"WORD_FACE\[4\] = \{([^}]*)\}", src).group(1).split(",")]
    scale = [int(v) for v in re.search(r"WORD_SCALE\[4\] = \{([^}]*)\}", src).group(1).split(",")]
    return palette, face, scale


def blend(under, over, a):
    def part(u, o):
        n = (o - u) * a
        return u + (n // 255 if n >= 0 else -((-n) // 255))
    return tuple(part(u, o) for u, o in zip(under, over))


def draw_words(img, w, h, face, scale, text, x, y, colour):
    size, glyphs, cover = face
    base = y + (size * 4) // 5 * scale
    pen = x
    for ch in text:
        o = ord(ch) if 32 <= ord(ch) <= 126 else 32
        gw, gh, left, top, adv, at = glyphs[o - 32]
        for gy in range(gh):
            for gx in range(gw):
                a = cover[at + gy * gw + gx]
                if not a:
                    continue
                for sy in range(scale):
                    for sx in range(scale):
                        py, qx = base + (gy - top) * scale + sy, pen + (left + gx) * scale + sx
                        if 0 <= py < h and 0 <= qx < w:
                            img[py * w + qx] = colour if a == 255 else blend(img[py * w + qx], colour, a)
        pen += adv * scale


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("paint's words")
    with open(SRC, "wb") as f:
        f.write(marker())
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", SRC + ":home/pic/w.png"],
                       cwd=ROOT, capture_output=True, text=True)
    os.remove(SRC)
    c.add("a volume with a marked picture", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    said = ""
    got = None
    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "words-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def keys(text):
            for ch in text:
                if ch.isupper():
                    key("shift-" + ch.lower(), settle=0.08)
                else:
                    key(NAMED.get(ch, ch), settle=0.08)

        def said_since(mark, text, timeout=20):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        start = len(vm.serial())
        keys("/bin/paint /home/pic/w.png\n")
        said_since(start, "paint: opened /home/pic/w.png", 30)
        _, _, _, shot, seen = mon.wait_screen(
            "words-open", lambda w, h, px: count_in(px, w, SCREEN, BLUE) >= 380, timeout=20)
        w, h, px, _ = mon.screen("words-find")
        mark_box = box_of(w, h, px, BLUE)
        c.add("Paint opens with the mark on its paper", seen and mark_box is not None, shot)
        if not mark_box:
            return c.report(keep=keep)
        ox, oy = mark_box[0], mark_box[1]

        def click(x, y):
            mon.click(ox + x, oy + y)
            time.sleep(0.5)

        mark = len(vm.serial())
        key("t")
        c.add("T chooses words", said_since(mark, "paint: words", 10))
        click(60, 50)
        keys("zelr 4b\n")
        key("3")                       # the 20 pixel face
        click(60, 110)
        keys("WAX")
        key("backspace")
        click(60, 170)                 # ends WA, and starts these
        keys("Hi!\n")
        key("4")                       # the 20 pixel face at twice its size
        click(300, 50)
        keys("big\n")
        click(300, 160)
        keys("gone\n")
        said_since(mark, "paint: words gone", 10)
        key("ctrl-z")
        said_since(mark, "paint: undone", 10)
        key("ctrl-s")
        saved = said_since(mark, "paint: saved /home/pic/w.png", 15)
        said = vm.serial()[start:]
        end = time.time() + 30
        while saved and got is None and time.time() < end:
            try:
                data = Fat16(IMAGE).read_file("/home/pic/w.png")
            except Exception:
                data = None
            got = png_pixels(data) if data else None
            time.sleep(0.5)
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    lines = re.findall(r"paint: words (.*), (\d+)x(\d+) at (\d+),(\d+)\r?\n", said)
    told = {t: (int(wd), int(ht), int(x), int(y)) for t, wd, ht, x, y in lines}
    c.add("the letters that choose tools went into the words: 'zelr 4b' (%s)" % sorted(told),
          "zelr 4b" in told)
    c.add("Backspace took a letter back, and a click ended them: 'WA'", "WA" in told and "WAX" not in told)
    c.add("the sizes: 15 bold, then 20, then 40",
          told.get("zelr 4b", (0, 0))[1] == 15 and told.get("WA", (0, 0))[1] == 20
          and told.get("Hi!", (0, 0))[1] == 20 and told.get("big", (0, 0))[1] == 40)
    c.add("said where each was put, in paper coordinates",
          told.get("zelr 4b", (0, 0, 0, 0))[2:] == (60, 50) and told.get("big", (0, 0, 0, 0))[2:] == (300, 50))
    c.add("the last words, taken back with ctrl+Z", "gone" in told and "paint: undone" in said)

    if got is None:
        c.add("the picture was saved and read back", False)
        return c.report(keep=keep)
    pw, ph, rgb, _ = got
    palette, wface, wscale = paint_tables()
    fs = faces()
    colour = palette[2]
    img = [PAPER] * (pw * ph)
    for y in range(20):
        for x in range(20):
            img[y * pw + x] = BLUE
    inked = []
    for text, brush in (("zelr 4b", 1), ("WA", 2), ("Hi!", 2), ("big", 3)):
        if text not in told:
            continue
        before = list(img)
        _, _, x, y = told[text]
        draw_words(img, pw, ph, fs[wface[brush]], wscale[brush], text, x, y, colour)
        inked.append(sum(1 for a, b in zip(before, img) if a != b))
    c.add("each set of words put ink on the paper (%s pixels)" % inked, len(inked) == 4 and min(inked) > 30)
    wrong = [(i % pw, i // pw) for i in range(pw * ph) if tuple(rgb[i * 3:i * 3 + 3]) != img[i]]
    if wrong:
        print("      | %d pixels differ, the first at %s" % (len(wrong), wrong[:6]))
    c.add("the saved picture is the paper with exactly those words on it, drawn as face.h draws them",
          not wrong)

    if not keep:
        try:
            os.remove(IMAGE)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
