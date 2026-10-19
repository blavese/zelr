"""Pages on this machine: the browser on file: addresses, and a page off the
network kept out of them.

A small site is put in /home/site on a fresh volume (8.3 names, as mkfat writes them): a page whose sheet and
picture sit beside it, a second page, and a picture. The browser opens it
by its path, the way Files hands a file over; the page's own colour comes
from its sheet and its picture is drawn, both read from the disk relative to
the page. A typed path is a file, a folder is listed as a page of links, and
Files opens a page in the browser. The browser says on the console what it
loaded ("browser: ADDRESS -- TITLE -- ..."), which each step waits for; the
screen is then counted by the page's colours.

A page on the disk asks for the page beside it every way a script can:
fetch, XMLHttpRequest, and a worker's fetch (which the browser answers by
another road). It paints itself red if any got it, blue if all were refused
and the worker ran, and yellow if the worker never answered; it must come out
blue, its own picture beside it still drawn.

Then a page from the web server on this side whose script fetches a file
from the machine and paints itself red if it got one and blue if it was
refused, and which shows a picture of one, green if it were drawn. It must
come out blue with no green.

  python tools/localcheck.py [--keep]
"""
import os
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT          # noqa: E402
from webserver import Server                                           # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "localcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "localcheck.%d" % os.getpid())

TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
PAGE_BG = (0xC0, 0x30, 0x70)            # the site's sheet paints the page this
GREEN = (0x20, 0xC0, 0x60)              # the site's picture
RED = (0xE0, 0x30, 0x40)                # the peeking page, had it read a file
BLUE = (0x30, 0x50, 0xE0)               # and refused
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret", ":": "shift-semicolon"}

INDEX = b"""<!doctype html>
<html><head><title>A page on this machine</title>
<link rel="stylesheet" href="style.css"></head>
<body><h1>Read off the disk</h1>
<p><img src="pic.png" width="40" height="30"></p>
<p><a href="page2.htm">the second page</a></p>
</body></html>
"""
PAGE2 = b"""<!doctype html><html><head><title>The second page</title></head><body><p>two</p></body></html>
"""
# A page on the disk whose scripts ask for a file beside it, three ways:
# all refused, it is blue; any read, red; the worker silent, yellow. Its own
# picture is the browser's to read, and is drawn.
PEEK = b"""<!doctype html><html><head><title>A page here that peeks</title></head>
<body style="margin:0;min-height:600px"><img src="pic.png" width="40" height="30">
<script>
var said = {};
function told(way, n) {
  said[way] = n;
  if (!("fetch" in said && "xhr" in said && "worker" in said)) return;
  var got = said.fetch > 0 || said.xhr > 0 || said.worker > 0;
  document.body.style.background = got ? "#e03040" : said.worker < 0 ? "#e0c020" : "#3050e0";
}
fetch("index.htm").then(function (r) { return r.text(); })
  .then(function (t) { told("fetch", t.length); }, function () { told("fetch", 0); });
var x = new XMLHttpRequest();
x.onload = function () { told("xhr", (x.responseText || "").length); };
x.onerror = function () { told("xhr", 0); };
x.open("GET", "index.htm");
x.send();
try {
  var w = new Worker("peekw.js");
  w.onmessage = function (e) { told("worker", e.data); };
  w.onerror = function () { told("worker", -1); };
} catch (e) { told("worker", -1); }
</script></body></html>
"""
PEEKW = b"""fetch("index.htm").then(function (r) { return r.text(); })
  .then(function (t) { postMessage(t.length); }, function () { postMessage(0); });
"""
YELLOW = (0xE0, 0xC0, 0x20)             # the peeking page's worker never answered
STYLE = b"body { background: #c03070; color: white; margin: 0; min-height: 600px }\n"


def png(path, w, h, rgb):
    rows = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("pages on this machine")
    os.makedirs(WORK, exist_ok=True)
    files = {"index.htm": INDEX, "page2.htm": PAGE2, "style.css": STYLE, "peek.htm": PEEK, "peekw.js": PEEKW}
    for name, body in files.items():
        with open(os.path.join(WORK, name), "wb") as f:
            f.write(body)
    png(os.path.join(WORK, "pic.png"), 40, 30, GREEN)
    args = [os.path.join(WORK, n) + ":home/site/" + n for n in list(files) + ["pic.png"]]
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a small site on it", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    with Server() as srv:
        vm = Guest(IMAGE, memory=256, reuse=True, keep=True, extra=srv.qemu_args())
        try:
            vm.wait_boot()
            vm.type("desktop\n")
            mon = vm.monitor()
            mon.move_to(1000, 20)
            _, _, _, shot, up = mon.wait_screen(
                "local-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
            c.add("the desktop comes up with its terminal", up, shot)
            time.sleep(1)

            def keys(text):
                for ch in text:
                    mon.send("sendkey %s" % NAMED.get(ch, ch), settle=0.06)

            def said_since(mark, text, timeout=40):
                end = time.time() + timeout
                while time.time() < end:
                    if text.lower() in vm.serial()[mark:].lower():   # mkfat's names are 8.3
                        return True
                    time.sleep(0.2)
                return False

            mark = len(vm.serial())
            keys("/bin/browser /home/site/index.htm\n")
            loaded = said_since(mark, "browser: file:///home/site/index.htm -- A page on this machine", 60)
            _, _, _, shot, drawn = mon.wait_screen(
                "local-index", lambda w, h, px: count_in(px, w, SCREEN, PAGE_BG) > 100000
                and abs(count_in(px, w, SCREEN, GREEN) - 40 * 30) < 120, timeout=30)
            c.add("a page opened by its path loads, as a file: address", loaded, shot)
            c.add("its sheet and its picture are read from beside it", drawn, shot)

            # A path typed into the bar, which ctrl+l gives the keyboard.
            mark = len(vm.serial())
            mon.send("sendkey ctrl-l", settle=0.3)
            keys("/home/site/page2.htm\n")
            c.add("a path typed into the bar is a file", said_since(mark, "browser: file:///home/site/page2.htm -- The second page"))

            mark = len(vm.serial())
            mon.send("sendkey ctrl-l", settle=0.3)
            keys("/home/site/\n")
            c.add("a folder is shown as a page of what is in it",
                  said_since(mark, "browser: file:///home/site/ -- /home/site/"))

            # A page on the disk is no different: its scripts are refused a
            # file every way they ask (a page saved from the web is a page on
            # the disk), and its own picture still drawn.
            mark = len(vm.serial())
            mon.send("sendkey ctrl-l", settle=0.3)
            keys("/home/site/peek.htm\n")
            seen = said_since(mark, "browser: file:///home/site/peek.htm -- A page here that peeks")
            _, _, _, shot, refused = mon.wait_screen(
                "local-peekhere", lambda w, h, px: count_in(px, w, SCREEN, BLUE) > 100000, timeout=40)
            w, h, px, _, _ = mon.wait_screen("local-peekhere2", lambda w, h, px: True)
            c.add("a page on the disk is refused a file its scripts ask for, by fetch, XMLHttpRequest "
                  "and a worker (red %d, yellow %d)" % (count_in(px, w, SCREEN, RED), count_in(px, w, SCREEN, YELLOW)),
                  seen and refused and count_in(px, w, SCREEN, RED) < 100, shot)
            c.add("and its own picture is still drawn", abs(count_in(px, w, SCREEN, GREEN) - 40 * 30) < 120, shot)

            # From the web: a page whose script asks for a file here.
            mark = len(vm.serial())
            mon.send("sendkey ctrl-l", settle=0.3)
            keys("http://%s/peek\n" % srv.host)
            seen = said_since(mark, "a page that peeks", 60)
            _, _, _, shot, refused = mon.wait_screen(
                "local-peek", lambda w, h, px: count_in(px, w, SCREEN, BLUE) > 100000, timeout=40)
            w, h, px, _, _ = mon.wait_screen("local-peek2", lambda w, h, px: True)
            c.add("a page off the network is refused a file its script asks for (red %d, blue %d)"
                  % (count_in(px, w, SCREEN, RED), count_in(px, w, SCREEN, BLUE)),
                  seen and refused and count_in(px, w, SCREEN, RED) < 100, shot)
            c.add("and a picture of one is not drawn", count_in(px, w, SCREEN, GREEN) < 50, shot)
            mon.send("sendkey alt-q", settle=1.0)

            # Files, which sent a page to the text editor.
            mark = len(vm.serial())
            keys("/bin/files /home/site\n")
            listed = said_since(mark, "files: /home/site", 30)
            chosen = False
            for _ in range(4):
                if not listed:
                    break
                mon.send("sendkey down", settle=0.5)
                if said_since(mark, "files: index.htm - page", 3):
                    chosen = True
                    break
            c.add("Files calls an html file a page", chosen)
            mark = len(vm.serial())
            mon.send("sendkey ret", settle=0.5)
            c.add("and opens it in the browser",
                  said_since(mark, "browser: file:///home/site/index.htm -- A page on this machine", 60))
        finally:
            vm.stop()

    for n in list(files) + ["pic.png"]:
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
