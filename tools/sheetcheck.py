"""Sheet: cells, formulas worked out, CSV kept and read.

A CSV written by Python's csv module (quotes, doubled quotes, a line break
in a field, \\r\\n line ends) is opened from Files; a new sheet is typed in
over the serial line, its first cell changed so everything that depends on
it is worked out again, each cell visited with ctrl+G and what it says on
the console compared; it is saved, the file read off the disk, and opened
again.

  - the launcher's search finds Sheet, which starts on a new sheet;
  - Files opens a .csv in Sheet, and the reader takes Python's CSV;
  - sums, functions over ranges, errors (#div/0, #cycle both ways, #value,
    #ref, #name, #error), words, COUNT passing errors over;
  - a change to A1 reaches everything that depends on it;
  - saved as CSV, formulas as typed, a field with a comma quoted;
  - opened again, the same answer.

  python tools/sheetcheck.py [--keep]
"""
import csv
import io
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402
from deskcheck import DOCK_BADGE                             # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "sheetcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "sheetcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}

IN_ROWS = [["n", "sq"], ["1", "=A2*A2"], ["2", "=A3*A3"], ["total", "=SUM(B2:B3)"],
           ['say "hi", ok', "=A5"], ["two\nlines"]]

TYPED = [
    "2\t3\t=A1+B1",
    "10\t=A2*B1\t=SUM(A1:B2)",
    "=A1/0\t=AVG(A1:A2)\t=C3",
    "hi, there\t=A4+1\t=MAX(A1:C2)-MIN(A1:B2)",
    "=(1+2)*3-4/8\t=1/3\t=2*-3",
    "=COUNT(A1:C3)\t=Z200+1\t=A0",
    "=FOO(A1)\t=1+\t=sum(a1:b1)",
    "=B8\t=A8+1",
]

# After A1 is changed to 7: what each cell says.
WANT = [
    ("A1", "7"),
    ("C1", "=A1+B1 is 10"),
    ("B2", "=A2*B1 is 30"),
    ("C2", "=SUM(A1:B2) is 50"),
    ("A3", "=A1/0 is #div/0"),
    ("B3", "=AVG(A1:A2) is 8.5"),
    ("C3", "=C3 is #cycle"),
    ("A4", "hi, there"),
    ("B4", "=A4+1 is #value"),
    ("C4", "=MAX(A1:C2)-MIN(A1:B2) is 47"),
    ("A5", "=(1+2)*3-4/8 is 8.5"),
    ("B5", "=1/3 is 0.333333"),
    ("C5", "=2*-3 is -6"),
    ("A6", "=COUNT(A1:C3) is 7"),
    ("B6", "=Z200+1 is 1"),
    ("C6", "=A0 is #ref"),
    ("A7", "=FOO(A1) is #name"),
    ("B7", "=1+ is #error"),
    ("C7", "=sum(a1:b1) is 10"),
    ("A8", "=B8 is #cycle"),
    ("B8", "=A8+1 is #cycle"),
]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("sheet")
    os.makedirs(WORK, exist_ok=True)
    src = os.path.join(WORK, "in")
    buf = io.StringIO()
    csv.writer(buf).writerows(IN_ROWS)
    with open(src, "wb") as f:
        f.write(buf.getvalue().encode())
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", src + ":home/s/in.csv"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a CSV that Python wrote", r.returncode == 0)
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
            "sheet-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def keys(text):
            for ch in text:
                key(NAMED.get(ch, ch), settle=0.06)

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

        mark = len(vm.serial())
        mon.click(*DOCK_BADGE)
        time.sleep(1.5)
        keys("sheet\n")
        c.add("the launcher's search finds Sheet, which starts on a new sheet",
              said_since(mark, "sheet: new /home/sheet.csv", 30))
        key("alt-q", settle=1.5)

        mark = len(vm.serial())
        vm.type("/bin/files /home/s\n")
        said_since(mark, "files: /home/s", 30)
        mark = len(vm.serial())
        key("down")
        c.add("Files calls a .csv a sheet", said_since(mark, "files: in.csv - sheet", 10))
        mark = len(vm.serial())
        key("ret")
        c.add("and opens it in Sheet: 6 rows", said_since(mark, "sheet: opened /home/s/in.csv, 6 rows", 30))
        time.sleep(1)
        got = [visit("B4"), visit("B5"), visit("A6")]
        c.add("Python's CSV read: a sum of formulas, doubled quotes, a line break in a field (%s)" % got,
              got == ["=SUM(B2:B3) is 5", '=A5 is say "hi", ok', "two lines"])

        mark = len(vm.serial())
        key("ctrl-o")
        time.sleep(0.3)
        key("ctrl-x")
        vm.type("/home/new.csv\n")
        c.add("ctrl+O on a file not there starts it new", said_since(mark, "sheet: new /home/new.csv", 15))

        typed = True
        for i, line in enumerate(TYPED):
            mark = len(vm.serial())
            vm.type(line + "\n")
            typed = typed and said_since(mark, "sheet: A%d empty" % (i + 2), 15)
        c.add("eight rows typed, Return going back to column A each time", typed)

        mark = len(vm.serial())
        got = visit("A1")
        vm.type("7\n")
        c.add("A1 changed from 2 to 7", got == "2" and said_since(mark, "sheet: A2 10", 15))

        wrong = []
        for name, want in WANT:
            got = visit(name)
            if got != want:
                wrong.append("%s %r, not %r" % (name, got, want))
        for w in wrong:
            print("      | " + w)
        c.add("every cell worked out again from A1: sums, functions, words and errors (%d wrong)" % len(wrong),
              not wrong)

        mark = len(vm.serial())
        key("ctrl-s")
        c.add("ctrl+S saves it: 8 rows", said_since(mark, "sheet: saved /home/new.csv, 8 rows", 15))

        mark = len(vm.serial())
        key("ctrl-o")
        time.sleep(0.3)
        key("ctrl-x")
        vm.type("/home/new.csv\n")
        opened = said_since(mark, "sheet: opened /home/new.csv, 8 rows", 15)
        c.add("opened again, C2 is still 50 and C4 47",
              opened and visit("C2") == "=SUM(A1:B2) is 50" and visit("C4") == "=MAX(A1:C2)-MIN(A1:B2) is 47")
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    try:
        saved = (Fat16(IMAGE).read_file("/home/new.csv") or b"").decode(errors="replace")
    except Exception:
        saved = ""
    want = "\n".join(["7,3,=A1+B1", "10,=A2*B1,=SUM(A1:B2)", "=A1/0,=AVG(A1:A2),=C3",
                      '"hi, there",=A4+1,=MAX(A1:C2)-MIN(A1:B2)', "=(1+2)*3-4/8,=1/3,=2*-3",
                      "=COUNT(A1:C3),=Z200+1,=A0", "=FOO(A1),=1+,=sum(a1:b1)", "=B8,=A8+1"]) + "\n"
    if saved != want:
        print("      | saved: %r" % saved[:400])
    c.add("the file is CSV: formulas as typed, the comma's field quoted", saved == want)
    back = list(csv.reader(io.StringIO(saved)))
    c.add("and Python's csv reads it back as typed", back and back[3][0] == "hi, there" and back[0] == ["7", "3", "=A1+B1"])

    try:
        os.remove(src)
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
