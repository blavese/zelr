"""Files bigger than a program's buffer, copied, moved and saved.

Three programs used to lose the end of a large file without a word:

  the terminal's cp read the whole file into a 16 KiB buffer and wrote back
  what fitted, and mv was cp followed by deleting the original

  Notes opened at most 64 KiB, so the file on screen was already short, and
  saving it -- which is what anybody does after opening a file -- wrote the
  short copy over the whole one

Each is driven here the way a person would, on a volume seeded with files
too big for the old buffers, and the result is read back off the disk image
on this side with tools/readfat.py, byte for byte against what was put there.

  python tools/bigfilecheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, centre_of, ROOT   # noqa: E402
from readfat import Fat16                                          # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "bigfile.%d.img" % os.getpid())
BIG_SRC = os.path.join(ROOT, "bigfile.%d.big" % os.getpid())
HUGE_SRC = os.path.join(ROOT, "bigfile.%d.huge" % os.getpid())

# The terminal's page once a `theme` has been obeyed: the sign that the
# command typed before it has finished, since the terminal runs them in turn.
AMBER = (0x14, 0x0E, 0x04)
PAPER = (0xF2, 0xEE, 0xE4)
PAGE = (120, 120, 700, 460)

# Notes' Save button is the one block of the accent colour on the left of its
# toolbar, found in a screenshot rather than remembered: the window cascades
# from wherever the terminal is, and a remembered position is how these
# harnesses break. The default accent, indigo.
ACCENT = (0x6E, 0x8A, 0xE8)
TOOLBAR_AREA = (120, 80, 360, 180)


def text_of(size, seed):
    """Lines that differ from each other, so a piece missing from the middle
    or repeated is as visible as one missing from the end."""
    out = bytearray()
    n = 0
    while len(out) < size:
        out += b"line %06d of file %d: the quick brown fox\n" % (n, seed)
        n += 1
    return bytes(out[:size])


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("big files")

    big = text_of(40000, 1)           # past the terminal's 16 KiB
    huge = text_of(100000, 2)         # past Notes' 64 KiB
    with open(BIG_SRC, "wb") as f:
        f.write(big)
    with open(HUGE_SRC, "wb") as f:
        f.write(huge)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768",
                        BIG_SRC + ":home/big.txt", HUGE_SRC + ":home/huge.txt"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with two large files on it", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        time.sleep(6)
        mon = vm.monitor()

        # The terminal has the focus, and what is typed on the serial line
        # reaches it -- but not while it is busy: a line typed during a
        # copy loses characters, and the next command runs into this one.
        # So one command at a time, each followed by a change of colour that
        # is waited for, which says the command before it has finished.
        def in_terminal(cmd, colour, rgb, name):
            vm.type(cmd + "\n")
            time.sleep(3)
            vm.type("theme %s\n" % colour)
            _, _, _, shot, ok = mon.wait_screen(
                name, lambda w, h, px: count_in(px, w, PAGE, rgb) > 50000, timeout=60)
            return ok, shot

        done1, shot1 = in_terminal("cp /home/big.txt /home/copy.txt", "amber", AMBER, "big-cp")
        done2, shot2 = in_terminal("cp /home/big.txt /home/gone.txt", "paper", PAPER, "big-cp2")
        done3, shot3 = in_terminal("mv /home/gone.txt /tmp/moved.txt", "amber", AMBER, "big-mv")
        c.add("the terminal ran the copies", done1 and done2 and done3,
              shot3 if not done3 else (shot2 if not done2 else shot1))

        # Notes, on a file bigger than it used to hold: one character typed
        # at the start, then saved. The character is what makes this a check
        # that a save happened at all -- a click that missed the button would
        # leave the file as it was, which is also what a working save of an
        # unchanged file looks like.
        vm.type("/bin/notes /home/huge.txt\n")
        save = None
        for _ in range(20):
            time.sleep(1)
            w, h, px, ppm = mon.screen("big-notes")
            try:
                os.remove(ppm)
            except OSError:
                pass
            save = centre_of(px, w, h, ACCENT, min_pixels=300, within=TOOLBAR_AREA)
            if save:
                break
        c.add("Notes opens with its Save button on screen", save is not None)
        if save:
            mon.send("sendkey x", settle=0.5)
            time.sleep(1)
            mon.click(*save)
            time.sleep(2)
            mon.click(*save)         # saving twice is harmless; missing once is not
            time.sleep(2)
        mon.send("sendkey alt-q", settle=1.0)
    finally:
        vm.stop()

    fs = Fat16(IMAGE)
    copy = fs.read_file("/home/copy.txt")
    c.add("cp copies a file larger than 16 KiB whole",
          copy is not None and copy == big)
    if copy is not None and copy != big:
        print("      copy.txt is %d bytes of %d" % (len(copy), len(big)))

    moved = fs.read_file("/tmp/moved.txt")
    c.add("mv into another directory arrives whole", moved is not None and moved == big)
    c.add("and only then is the original gone", fs.read_file("/home/gone.txt") is None)
    c.add("the file copied from is untouched", fs.read_file("/home/big.txt") == big)

    saved = fs.read_file("/home/huge.txt")
    want = b"x" + huge
    c.add("Notes opens, edits and saves a file larger than 64 KiB without losing its end",
          saved is not None and saved == want)
    if saved is not None and saved != want:
        print("      huge.txt is %d bytes after the save, wanted %d%s"
              % (len(saved), len(want), " (unchanged: the save never happened)"
                 if saved == huge else ""))

    for junk in (BIG_SRC, HUGE_SRC) + (() if keep else (IMAGE,)):
        try:
            os.remove(junk)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
