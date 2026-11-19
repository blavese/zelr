"""Words selected in the browser, and copied.

A page on a fresh volume, opened as a file (no network needed): two lines
of words, each word in a span of its own colour so it can be found on the
screen, the last a link. The browser says on the console what a selection
holds ("browser selection: 5 words: beta gamma delta/epsilon zeta", a slash
where a line ends) and what it copied; the clipboard is read back at the
end from the kernel's shell, after alt+Escape leaves the desktop, as
clipcheck reads it.

  - a drag from beta to zeta selects the words between, across the line,
    and draws them selected: beta's colour goes under the selection's and
    alpha's stays;
  - ctrl+C copies them, a line apart where the page has them on lines
    apart, and the clipboard holds exactly that;
  - a click in place puts the selection away;
  - a drag that ends on the link selects and does not follow it;
  - a double click selects the word under it, and the page hears dblclick
    (a block turns green); a triple click selects the word's paragraph;
  - ctrl+A selects every word;
  - the right button's menu offers "copy" and "look it up" first, and
    "look it up" searches for the words.

  python tools/selectcheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT          # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "selectcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "selectcheck.%d" % os.getpid())

TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
SELECTED = (0xAC, 0xCE, 0xF7)           # browser.c SEL_COLOUR
HEARD = (0x10, 0xA0, 0x10)              # the page's block once it heard dblclick
WORDS = [("alpha", (0xE0, 0x30, 0x40)), ("beta", (0x30, 0xC0, 0x70)), ("gamma", (0xE0, 0xC0, 0x20)),
         ("delta", (0xC0, 0x70, 0xC0)), ("epsilon", (0x40, 0x80, 0xC0)), ("zeta", (0xE0, 0x80, 0x20)),
         ("eta", (0x70, 0xB0, 0xB0)), ("theta", (0xB0, 0x90, 0x60))]
COLOUR = dict(WORDS)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}
UI_ROW, UI_GAP = 26, 6                  # the right button's menu (menucheck)


def page():
    def span(w):
        r, g, b = COLOUR[w]
        return '<span style="background:#%02x%02x%02x">%s</span>' % (r, g, b, w)
    one = " ".join(span(w) for w in ("alpha", "beta", "gamma", "delta"))
    two = " ".join(span(w) for w in ("epsilon", "zeta", "eta"))
    r, g, b = COLOUR["theta"]
    return ("""<!doctype html><html><head><title>page s</title><style>
body { margin: 0; background: #ffffff; font-size: 28px }
p { margin: 24px; line-height: 48px }
a { color: #202020; background: #%02x%02x%02x }
#heard { height: 24px; margin: 24px; background: #101010 }
</style></head><body><p>%s</p><p>%s <a href="t.htm">theta</a></p><div id="heard"></div>
<script>document.addEventListener("dblclick", function (e) {
  if (e.detail == 2) document.getElementById("heard").style.background = "#10a010"; });</script>
</body></html>
""" % (r, g, b, one, two)).encode()


PAGE_T = b"""<!doctype html><html><head><title>page t</title></head><body><p>t</p></body></html>"""


def box_of(w, h, px, rgb):
    """The box around every pixel of a colour, or None."""
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for y in range(0, h, 2):
        row = y * w * 3
        for x in range(0, w, 2):
            i = row + x * 3
            if px[i] == rgb[0] and px[i + 1] == rgb[1] and px[i + 2] == rgb[2]:
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    return None if x1 < 0 else (x0, y0, x1, y1)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("words selected in the browser")
    os.makedirs(WORK, exist_ok=True)
    files = {"s.htm": page(), "t.htm": PAGE_T}
    for name, body in files.items():
        with open(os.path.join(WORK, name), "wb") as f:
            f.write(body)
    args = [os.path.join(WORK, n) + ":home/sel/" + n for n in files]
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with the page on it", r.returncode == 0)
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
            "sel-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def keys(text):
            for ch in text:
                mon.send("sendkey %s" % NAMED.get(ch, ch), settle=0.06)

        def said_since(mark, text, timeout=40):
            end = time.time() + timeout
            while time.time() < end:
                if text.lower() in vm.serial()[mark:].lower():
                    return True
                time.sleep(0.2)
            return False

        def line_since(mark, prefix):
            out = vm.serial()[mark:]
            at = out.find(prefix)
            return "" if at < 0 else out[at:].split("\n")[0].strip()

        def boxes():
            w, h, px, _ = mon.screen("sel-find")
            return {name: box_of(w, h, px, rgb) for name, rgb in WORDS}

        def centre(b):
            return ((b[0] + b[2]) // 2, (b[1] + b[3]) // 2)

        def shown(name):
            w, h, px, _ = mon.screen("sel-count")
            return count_in(px, w, SCREEN, COLOUR[name]), count_in(px, w, SCREEN, SELECTED)

        mark = len(vm.serial())
        keys("/bin/browser /home/sel/s.htm\n")
        loaded = said_since(mark, "browser: file:///home/sel/s.htm -- page s", 60)
        time.sleep(2)
        at = boxes()
        c.add("the page opens with its eight words where they can be found",
              loaded and all(at[n] for n, _ in WORDS), str(at))
        if not all(at[n] for n, _ in WORDS):
            return c.report(keep=keep)
        beta_before, _ = shown("beta")

        # A drag from beta to zeta.
        mark = len(vm.serial())
        mon.drag(centre(at["beta"]), centre(at["zeta"]))
        said = said_since(mark, "browser selection: ", 20)
        time.sleep(0.5)
        line = line_since(mark, "browser selection: ")
        c.add("a drag from beta to zeta selects the five words between, across the line",
              said and line == "browser selection: 5 words: beta gamma delta/epsilon zeta", line)
        # Counted in each word's own box: the selection is drawn behind the
        # words, over the box a span's background fills.
        w, h, px, shot = mon.screen("sel-drawn")
        lit = {n: count_in(px, w, (b[0], b[1], b[2] + 1, b[3] + 1), SELECTED) for n, b in at.items()}
        c.add("and draws them selected, and only them (%s)" % ", ".join("%s %d" % (n, lit[n]) for n, _ in WORDS),
              all(lit[n] > 100 for n in ("beta", "gamma", "delta", "epsilon", "zeta"))
              and all(lit[n] < 10 for n in ("alpha", "eta", "theta")), shot)

        # ctrl+C; what it put on the clipboard is read at the end.
        mark = len(vm.serial())
        mon.send("sendkey ctrl-c", settle=0.5)
        copied = said_since(mark, "browser selection: copied 29 characters", 20)

        # A click in place puts it away.
        w, h, px, _ = mon.screen("sel-blank")
        blank = (at["delta"][2] + 120, at["delta"][1] + 10)
        mon.click(*blank)
        _, _, _, shot, back = mon.wait_screen(
            "sel-cleared", lambda w, h, px: count_in(px, w, SCREEN, COLOUR["beta"]) >= beta_before - 20
            and count_in(px, w, SCREEN, SELECTED) < 50, timeout=15)
        c.add("a click in place puts the selection away", back, shot)

        # A drag that ends on the link selects, and does not follow it: a
        # link is followed on a release over it, which this release is.
        mark = len(vm.serial())
        mon.drag(centre(at["eta"]), centre(at["theta"]))
        said = said_since(mark, "browser selection: ", 20)
        time.sleep(3.0)
        line = line_since(mark, "browser selection: ")
        c.add("a drag that ends on a link selects (%s) and does not follow it" % line,
              said and line == "browser selection: 2 words: eta theta"
              and "file:///home/sel/t.htm" not in vm.serial()[mark:])

        # A double click on gamma, a triple on zeta: presses half a second
        # apart at most in the guest, the pointer still between them.
        def presses(where, n):
            mon.move_to(*where)
            time.sleep(0.4)
            for _ in range(n):
                mon.send("mouse_button 1", settle=0.12)
                mon.send("mouse_button 0", settle=0.12)

        mark = len(vm.serial())
        presses(centre(at["gamma"]), 2)
        said = said_since(mark, "browser selection: ", 20)
        time.sleep(0.5)
        line = line_since(mark, "browser selection: ")
        _, _, _, shot, heard = mon.wait_screen(
            "sel-heard", lambda w, h, px: count_in(px, w, SCREEN, HEARD) > 1000, timeout=15)
        c.add("a double click selects the word under it (%s), and the page hears dblclick" % line,
              said and line == "browser selection: 1 word: gamma" and heard, shot)
        mark = len(vm.serial())
        presses(centre(at["zeta"]), 3)
        said_since(mark, "browser selection: 4 words", 20)
        time.sleep(0.5)
        out = vm.serial()[mark:]
        c.add("a triple click selects the word's paragraph",
              "browser selection: 4 words: epsilon zeta eta theta" in out,
              " / ".join(l.strip() for l in out.split("\n") if "browser selection" in l))

        # ctrl+A.
        mark = len(vm.serial())
        mon.send("sendkey ctrl-a", settle=0.5)
        said = said_since(mark, "browser selection: ", 20)
        line = line_since(mark, "browser selection: ")
        c.add("ctrl+A selects every word",
              said and line == "browser selection: 8 words: alpha beta gamma delta/epsilon zeta eta theta", line)

        # The right button: copy and look it up, first.
        spot = (blank[0], blank[1] + 60)
        mark = len(vm.serial())
        mon.click(spot[0], spot[1], button=2)
        said_since(mark, "browser menu: ", 20)
        time.sleep(0.3)
        rows = line_since(mark, "browser menu: ")[14:].split("; ")
        c.add("with words selected the menu offers to copy them and look them up, first",
              rows[:2] == ["copy", "look it up"], "; ".join(rows))
        mark = len(vm.serial())
        if rows[:2] == ["copy", "look it up"]:
            mon.click(spot[0] + 60, spot[1] + UI_GAP + UI_ROW + UI_ROW // 2)
        looked = said_since(mark, "browser menu: looked up alpha beta gamma delta epsilon zeta eta theta", 20)
        went = said_since(mark, "browser: https://lite.duckduckgo.com/lite/?q=alpha", 120)
        c.add("\"look it up\" searches for the words", looked and went)

        # The clipboard, from the kernel's shell once the desktop has let go
        # of the console (alt+Escape always leaves): what ctrl+C put there,
        # nothing since having copied anything.
        mon.send("sendkey alt-esc")
        handed_back = vm.wait_serial("back at the shell", timeout=30)
        out = vm.run("cat /sys/clipboard").replace("\r", "")
        tail = out.split("cat /sys/clipboard")[-1]
        c.add("ctrl+C copied the words, a line apart where the page breaks the line",
              copied and handed_back and "beta gamma delta\nepsilon zeta" in tail, tail[:120])
    finally:
        vm.stop()

    for n in files:
        try:
            os.remove(os.path.join(WORK, n))
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
