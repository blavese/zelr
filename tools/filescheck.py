"""The file manager's copy, cut and rename, done the way a person does them.

Files had no harness of its own. Its copy and rename were fixed for large
files in 0.40.0 and checked only through the terminal's cp and mv, which
share nothing with it but the system calls. This drives Files itself with
the keyboard -- the arrows, Enter and Backspace to move about, ctrl+c,
ctrl+x and ctrl+v, and F2 to rename in place -- on a volume seeded with a
folder and two files, one of them bigger than the buffer Files copies
through, and reads the result back off the disk image on this side with
tools/readfat.py, byte for byte.

Each step is waited for on the disk rather than for a time: QEMU writes the
image through to the file, so the file this side has what the guest wrote
once the write returns, and the check polls it until the change arrives.

  python tools/filescheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "files.%d.img" % os.getpid())
LETTER_SRC = os.path.join(ROOT, "files.%d.letter" % os.getpid())
ZETA_SRC = os.path.join(ROOT, "files.%d.zeta" % os.getpid())
BOX_SRC = os.path.join(ROOT, "files.%d.box" % os.getpid())

# Files' window, in the default theme: its list is painted in a near white
# nothing else on this desktop uses over such an area, and a selected row in
# a pale blue of its own.
WINDOW = (0xFA, 0xFA, 0xFC)
SELECTED = (0xDF, 0xE4, 0xF5)
TERMINAL = (0x10, 0x14, 0x1A)          # the terminal's default background
SCREEN = (0, 0, 1024, 768)


def text_of(size, seed):
    """Lines that differ from each other, so a piece missing from the middle
    or repeated is as visible as one missing from the end."""
    out = bytearray()
    n = 0
    while len(out) < size:
        out += b"line %06d of file %d: pack my box with five dozen jugs\n" % (n, seed)
        n += 1
    return bytes(out[:size])


def read(path):
    try:
        return Fat16(IMAGE).read_file(path)
    except Exception:          # a directory caught half written: look again
        return None


def wait_disk(test, timeout=30):
    """Until the image this side says `test` is true."""
    end = time.time() + timeout
    while time.time() < end:
        if test():
            return True
        time.sleep(0.5)
    return test()


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("files")

    letter = text_of(40000, 1)          # past the 16 KiB Files copies through
    zeta = text_of(3000, 2)
    box = b"a folder with something in it\n"
    for path, data in ((LETTER_SRC, letter), (ZETA_SRC, zeta), (BOX_SRC, box)):
        with open(path, "wb") as f:
            f.write(data)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768",
                        LETTER_SRC + ":home/letter.txt", ZETA_SRC + ":home/zeta.txt",
                        BOX_SRC + ":home/box/readme.txt"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a folder and two files in /home", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)               # out of everything compared

        # Started from the terminal, which has the keyboard when the desktop
        # comes up -- and only once it is up, because a key with no window to
        # go to is dropped. On /home, whose listing is box, letter.txt, notes
        # (which the first boot puts there) and zeta.txt: folders first, then
        # files, each alphabetical.
        _, _, _, shot, up = mon.wait_screen(
            "files-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000,
            timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)                      # the terminal takes its first key
        vm.type("/bin/files /home\n")
        _, _, _, shot, up = mon.wait_screen(
            "files-open", lambda w, h, px: count_in(px, w, SCREEN, WINDOW) > 50000, timeout=60)
        c.add("Files opens", up, shot)
        if not up:
            return c.report(keep=keep)

        def key(name, settle=0.25):
            mon.send("sendkey %s" % name, settle=settle)

        # A row selected, which is also the sign the window has the keyboard.
        key("down")
        _, _, _, shot, sel = mon.wait_screen(
            "files-row", lambda w, h, px: count_in(px, w, SCREEN, SELECTED) > 10000, timeout=20)
        c.add("the arrows select a row", sel, shot)

        # --- copy: letter.txt into box -----------------------------------------
        key("down")                        # letter.txt
        key("ctrl-c")
        key("up")                          # box
        key("ret")                         # into it
        key("ctrl-v")
        copied = wait_disk(lambda: read("/home/box/letter.txt") == letter)
        c.add("ctrl+c and ctrl+v copy a file bigger than the buffer, whole", copied)
        c.add("and the file copied from is untouched", read("/home/letter.txt") == letter)

        # --- rename: zeta.txt to omega.txt, in place ------------------------------
        key("backspace")                   # back up to /home
        for _ in range(4):                 # box, letter.txt, notes, zeta.txt
            key("down")
        started = time.time()
        key("f2")
        for _ in range(len("zeta.txt")):
            key("backspace", settle=0.02)
        for ch in "omega":
            key(ch, settle=0.02)
        key("dot", settle=0.02)
        for ch in "txt":
            key(ch, settle=0.02)
        key("ret")
        renamed = wait_disk(lambda: read("/home/omega.txt") == zeta)
        # How long eighteen keys typed quickly took to be acted on, which is
        # a measurement rather than a check: the desktop handed a program one
        # key per frame it drew, and now hands it all that are waiting.
        print("      the rename landed %.1f s after F2" % (time.time() - started))
        c.add("F2 renames a file where it is", renamed)
        c.add("and the old name is gone", read("/home/zeta.txt") is None)

        # --- cut: letter.txt into box, over the copy ---------------------------
        # The list is box, letter.txt, notes, omega.txt now, nothing selected.
        key("down"); key("down")           # letter.txt
        key("ctrl-x")
        key("up")                          # box
        key("ret")
        key("ctrl-v")
        moved = wait_disk(lambda: read("/home/letter.txt") is None)
        c.add("ctrl+x and ctrl+v move a file", moved)
        c.add("and it arrives whole", read("/home/box/letter.txt") == letter)
        c.add("beside what the folder already had", read("/home/box/readme.txt") == box)

        mon.send("sendkey alt-q", settle=1.0)
    finally:
        vm.stop()

    for junk in (LETTER_SRC, ZETA_SRC, BOX_SRC) + (() if keep else (IMAGE,)):
        try:
            os.remove(junk)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
