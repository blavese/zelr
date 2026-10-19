"""A download bigger than the browser holds at once.

The browser fetches a page or a file into room of eight megabytes; a file
bigger than that was "Not saved". Now what came first is written and the
rest asked for a stretch at a time with Range, and the file is read back
against a sum kept as it was written (browser.c save_download_rest).

The web server on this side sends twelve megabytes as big.bin, in parts
when asked, and the same as whole.bin, which it always sends whole, as a
server that does not do ranges does. On a 64 MB volume:

  - three megabytes, held at once but more than one write takes (the
    system refuses a write past a megabyte), are saved: they were "not
    saved";
  - whole.bin is not saved, and the browser says why;
  - big.bin is saved, all of it, the console says so, the server was asked
    for the rest in stretches from where the first part ended, and the file
    read off the disk image here is the server's, byte for byte.

  python tools/downloadcheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT          # noqa: E402
from readfat import Fat16                                              # noqa: E402
from webserver import Server, BIG_BIN                                  # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "download.%d.img" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret", ":": "shift-semicolon"}


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("a download bigger than the browser holds")
    # mkfat's size is in kilobytes, and it wants a file to put on the volume.
    note = IMAGE + ".txt"
    with open(note, "wb") as f:
        f.write(b"room for downloads\n")
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "65536", note + ":home/readme.txt"],
                       cwd=ROOT, capture_output=True, text=True)
    os.remove(note)
    c.add("a 64 MB volume", r.returncode == 0)
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
                "dl-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
            c.add("the desktop comes up with its terminal", up, shot)
            time.sleep(1)

            def keys(text):
                for ch in text:
                    mon.send("sendkey %s" % NAMED.get(ch, ch), settle=0.06)

            def said_since(mark, text, timeout=60):
                end = time.time() + timeout
                while time.time() < end:
                    if text in vm.serial()[mark:]:
                        return True
                    time.sleep(0.5)
                return False

            mark = len(vm.serial())
            keys("/bin/browser http://%s/dl/mid.bin\n" % srv.host)
            c.add("three megabytes are saved and read back",
                  said_since(mark, "saved /home/downloads/mid.bin, %d bytes, read back the same" % (3 << 20), 240))

            mark = len(vm.serial())
            mon.send("sendkey ctrl-l", settle=0.3)
            keys("http://%s/dl/whole.bin\n" % srv.host)
            c.add("a file too big that the server sends only whole is not saved, and says so",
                  said_since(mark, "-- whole.bin -- not saved, not sent in parts: whole.bin", 240))

            mark = len(vm.serial())
            mon.send("sendkey ctrl-l", settle=0.3)
            keys("http://%s/dl/big.bin\n" % srv.host)
            saved = said_since(mark, "saved /home/downloads/big.bin, %d bytes, read back the same" % len(BIG_BIN), 400)
            line = [ln for ln in vm.serial()[mark:].splitlines() if ln.startswith("browser: ") and "big.bin" in ln]
            c.add("one it sends in parts is saved, all of it, and read back (%s)" % (line[-1] if line else "nothing"),
                  saved)
            asked = srv.ranges()
            joined = bool(asked) and asked[-1][1] == len(BIG_BIN) - 1 and all(
                asked[i][1] + 1 == asked[i + 1][0] for i in range(len(asked) - 1))
            c.add("the rest asked for a stretch at a time, from where the first part ended (%s)" % asked,
                  joined and 0 < asked[0][0] < len(BIG_BIN))
            mon.send("sendkey alt-q", settle=1.0)
            time.sleep(2.0)
        finally:
            vm.stop()

    try:
        got = Fat16(IMAGE).read_file("/home/downloads/big.bin")
        gone = Fat16(IMAGE).read_file("/home/downloads/whole.bin")
        mid = Fat16(IMAGE).read_file("/home/downloads/mid.bin")
    except Exception as e:
        print("  could not read the image: %s" % e)
        got, gone, mid = None, b"", None
    c.add("the file on the disk is the server's, byte for byte (%s bytes)" % (len(got) if got else 0), got == BIG_BIN)
    c.add("and nothing of the one not saved is left", gone is None)
    c.add("and the three megabytes are the server's too", mid == BIG_BIN[:3 << 20])
    if not keep:
        try:
            os.remove(IMAGE)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
