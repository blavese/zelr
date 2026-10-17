"""Notes, the text editor, worked from its keys.

ctrl+a selects all of the text, ctrl+c copies it, ctrl+v pastes it, ctrl+s
saves and ctrl+z undoes. Each was typed into the text as the control
character it folds to -- ctrl+s put a 0x13 in the file and saved nothing --
and there was no undo at all. This opens Notes on a file seeded on the disk,
presses the chords through the monitor, and reads the file back off the disk
image on this side with tools/readfat.py after every save.

Each step is waited for on the disk rather than for a time, as filescheck's
are.

  python tools/notescheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, centre_of, ROOT   # noqa: E402
from readfat import Fat16                                                 # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "notes.%d.img" % os.getpid())
NOTE_SRC = os.path.join(ROOT, "notes.%d.note" % os.getpid())

NOTE = b"alpha beta\n"

TERMINAL = (0x10, 0x14, 0x1A)          # the terminal's default background
SCREEN = (0, 0, 1024, 768)
# Notes' Save button, in the accent, on its toolbar (as bigfilecheck finds it).
ACCENT = (0x6E, 0x8A, 0xE8)
TOOLBAR_AREA = (120, 80, 360, 180)


def read(path):
    try:
        return Fat16(IMAGE).read_file(path)
    except Exception:          # a directory caught half written: look again
        return None


def wait_disk(test, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if test():
            return True
        time.sleep(0.5)
    return test()


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("notes")

    with open(NOTE_SRC, "wb") as f:
        f.write(NOTE)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768",
                        NOTE_SRC + ":home/note.txt"], cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a short note on it", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)               # out of everything compared

        _, _, _, shot, up = mon.wait_screen(
            "notes-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)                      # the terminal takes its first key
        vm.type("/bin/notes /home/note.txt\n")
        _, _, _, shot, up = mon.wait_screen(
            "notes-open", lambda w, h, px: centre_of(px, w, h, ACCENT, min_pixels=300,
                                                     within=TOOLBAR_AREA) is not None, timeout=60)
        c.add("Notes opens", up, shot)
        if not up:
            return c.report(keep=keep)

        def key(name, settle=0.25):
            mon.send("sendkey %s" % name, settle=settle)

        # All of it copied and pasted after itself: page down drops the
        # selection and goes to the end, where the paste goes.
        key("ctrl-a")
        key("ctrl-c")
        key("pgdn")
        key("ctrl-v")
        key("ctrl-s")
        c.add("ctrl+a, ctrl+c, ctrl+v and ctrl+s copy the whole note after itself and save it",
              wait_disk(lambda: read("/home/note.txt") == NOTE + NOTE))

        key("ctrl-z")
        key("ctrl-s")
        c.add("ctrl+z takes the paste back", wait_disk(lambda: read("/home/note.txt") == NOTE))

        for ch in "zzz":
            key(ch, settle=0.05)
        key("ctrl-s")
        c.add("typing goes in where the cursor was put back", wait_disk(lambda: read("/home/note.txt") == NOTE + b"zzz"))

        key("ctrl-z")
        key("ctrl-s")
        c.add("and ctrl+z takes a run of typing back at once", wait_disk(lambda: read("/home/note.txt") == NOTE))

        mon.send("sendkey alt-q", settle=1.0)
    finally:
        vm.stop()

    for junk in (NOTE_SRC,) + (() if keep else (IMAGE,)):
        try:
            os.remove(junk)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
