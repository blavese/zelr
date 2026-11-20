"""Sheet: references that move when a formula is filled, copied and pasted.

A new sheet typed in over the serial line, then filled down (ctrl+D) and
right (ctrl+R), a cell copied and pasted (ctrl+C, ctrl+V) and one cut and
pasted (ctrl+X), each cell visited with ctrl+G and what it says on the
console compared, and the saved CSV read off the disk.

  - ctrl+D: =A1*2 below is =A2*2, then =A3*2, =A4*2;
  - $ keeps its part: =$A$1+A1 down is =$A$1+A2, =A$1+1 down stays;
  - ctrl+R: =A$1+1 to the right is =B$1+1;
  - a range moves at both ends unless fixed: =SUM($A$1:A5) down is
    =SUM($A$1:A6);
  - a copy pasted two rows down and one along moves by as much; one
    moved off the sheet's edge says #ref;
  - a cut pasted keeps its references where they were, as spreadsheets do.

  python tools/fillcheck.py [--keep]
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "fillcheck.%d.img" % os.getpid())
SEED = os.path.join(ROOT, "fillcheck.%d.txt" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)

TYPED = ["1\t=A1*2\t=$A$1+A1\t=A$1+1\t\t\t=SUM($A$1:A5)", "2", "3", "4", "5"]

WANT = [
    ("B2", "=A2*2 is 4"),
    ("B3", "=A3*2 is 6"),
    ("B4", "=A4*2 is 8"),
    ("C2", "=$A$1+A2 is 3"),
    ("D2", "=A$1+1 is 2"),
    ("E1", "=B$1+1 is 3"),
    ("G2", "=SUM($A$1:A6) is 15"),
    ("C4", "=B4*2 is 16"),
    ("A7", "=#ref*2 is #ref"),
    ("B5", "empty"),
    ("F5", "=A5*2 is 10"),
]

SAVED = ("1,=A1*2,=$A$1+A1,=A$1+1,=B$1+1,,=SUM($A$1:A5)\n"
         "2,=A2*2,=$A$1+A2,=A$1+1,,,=SUM($A$1:A6)\n"
         "3,=A3*2\n"
         "4,=A4*2,=B4*2\n"
         "5,,,,,=A5*2\n"
         "\n"
         "=#ref*2\n")


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("fill")
    with open(SEED, "w") as f:
        f.write("seed")
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", SEED + ":home/seed.txt"],
                       cwd=ROOT, capture_output=True, text=True)
    os.remove(SEED)
    c.add("a fresh volume", r.returncode == 0)
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
            "fill-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

        def visit(name):
            mark = len(vm.serial())
            key("ctrl-g")
            time.sleep(0.3)
            vm.type(name + "\n")
            m = None
            end = time.time() + 15
            while time.time() < end and not m:
                m = re.search(r"sheet: %s ([^\r\n]*)\r?\n" % name, vm.serial()[mark:])
                time.sleep(0.2)
            return m.group(1) if m else None

        def chord_at(name, chord, want):
            """Goes to a cell, presses the chord, and waits for what the cell
            then says."""
            visit(name)
            mark = len(vm.serial())
            key(chord)
            return said_since(mark, "sheet: %s %s" % (name, want), 10)

        mark = len(vm.serial())
        vm.type("/bin/sheet /home/r.csv\n")
        c.add("Sheet starts on a new sheet", said_since(mark, "sheet: new /home/r.csv", 30))
        time.sleep(1)
        typed = True
        for i, line in enumerate(TYPED):
            mark = len(vm.serial())
            vm.type(line + "\n")
            typed = typed and said_since(mark, "sheet: A%d empty" % (i + 2), 15)
        c.add("a row of formulas and a column of numbers typed", typed)

        c.add("ctrl+D fills =A1*2 down as =A2*2, =A3*2, =A4*2 and =A5*2",
              chord_at("B2", "ctrl-d", "=A2*2 is 4") and chord_at("B3", "ctrl-d", "=A3*2 is 6")
              and chord_at("B4", "ctrl-d", "=A4*2 is 8") and chord_at("B5", "ctrl-d", "=A5*2 is 10"))
        c.add("$A$1 stays and A1 moves: =$A$1+A2", chord_at("C2", "ctrl-d", "=$A$1+A2 is 3"))
        c.add("A$1 keeps its row going down: =A$1+1", chord_at("D2", "ctrl-d", "=A$1+1 is 2"))
        c.add("and moves its column going right (ctrl+R): =B$1+1", chord_at("E1", "ctrl-r", "=B$1+1 is 3"))
        c.add("a range's free end moves: =SUM($A$1:A6)", chord_at("G2", "ctrl-d", "=SUM($A$1:A6) is 15"))

        visit("B1")
        key("ctrl-c")
        c.add("B1 copied and pasted at C4 is =B4*2", chord_at("C4", "ctrl-v", "=B4*2 is 16"))
        c.add("and at A7, off the sheet's left edge, =#ref*2", chord_at("A7", "ctrl-v", "=#ref*2 is #ref"))

        visit("B5")
        mark = len(vm.serial())
        key("ctrl-x")
        cut = said_since(mark, "sheet: B5 empty", 10)
        c.add("B5 cut, and pasted at F5 still =A5*2", cut and chord_at("F5", "ctrl-v", "=A5*2 is 10"))

        wrong = []
        for name, want in WANT:
            got = visit(name)
            if got != want:
                wrong.append("%s %r, not %r" % (name, got, want))
        for w in wrong:
            print("      | " + w)
        c.add("every cell says what it should, visited again (%d wrong)" % len(wrong), not wrong)

        mark = len(vm.serial())
        key("ctrl-s")
        c.add("saved", said_since(mark, "sheet: saved /home/r.csv, 7 rows", 15))
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    try:
        saved = (Fat16(IMAGE).read_file("/home/r.csv") or b"").decode(errors="replace")
    except Exception:
        saved = ""
    if saved != SAVED:
        print("      | saved: %r" % saved[:400])
    c.add("the file holds the moved formulas", saved == SAVED)

    if not keep:
        try:
            os.remove(IMAGE)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
