"""Sheet's ctrl+T: rows put in order by a column.

A small table typed into a new sheet: a header, then seven rows of a name,
a score that is a number, words, an error or nothing, and a formula on its
own row. Sorted by the scores, again the other way, then by the names; after
each the names down column A and the formulas beside them are read from the
console, and the saved CSV is read back by Python's csv.

  - numbers smallest first, then words, then errors, empty cells last;
  - ties keep their order;
  - ctrl+T again on the same column goes largest first, errors first,
    empty still last;
  - words with case aside (apple before Banana);
  - each row moves whole, its formulas as a copy would be (=B3*2 in row 3
    is =B6*2 in row 6), and the header above the chosen row stays.

  python tools/ordercheck.py [--keep]
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

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "ordercheck.%d.img" % os.getpid())
SEED = os.path.join(ROOT, "ordercheck.%d.txt" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)

# name, score, and the formula beside it (written for its own row, {r}).
ROWS = [
    ("pear", "30", "=B{r}*2"),
    ("apple", "5", "=B{r}*2"),
    ("fig", "30", "=B{r}*2"),
    ("kiwi", "", "=B{r}*2"),
    ("Banana", "-2", "=B{r}*2"),
    ("cherry", "hello", '=B{r}&"!"'),
    ("date", "=1/0", "=B{r}"),
]
SHOWN = {"pear": "60", "apple": "10", "fig": "60", "kiwi": "0", "Banana": "-4", "cherry": "hello!",
         "date": "#div/0"}
BY_SCORE = ["Banana", "apple", "pear", "fig", "cherry", "date", "kiwi"]
BY_SCORE_DOWN = ["date", "cherry", "pear", "fig", "apple", "Banana", "kiwi"]
BY_NAME = ["apple", "Banana", "cherry", "date", "fig", "kiwi", "pear"]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("sheet's sorting")
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
            "order-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

        def column():
            """The names down A2 to A8, and whether each row's formula is its
            own row's and says what that row's should."""
            names, whole = [], True
            for i in range(2, 9):
                n = visit("A%d" % i)
                names.append(n)
                f = visit("C%d" % i)
                row = [x for x in ROWS if x[0] == n]
                if not row or f != "%s is %s" % (row[0][2].format(r=i), SHOWN[n]):
                    whole = False
                    print("      | C%d %r beside %r" % (i, f, n))
            return names, whole

        def sort_at(cell, want):
            visit(cell)
            mark = len(vm.serial())
            key("ctrl-t")
            said = said_since(mark, "sheet: sorted " + want, 10)
            visit("D1")                 # away, so the first cell read is said afresh
            return said

        mark = len(vm.serial())
        vm.type("/bin/sheet /home/o.csv\n")
        c.add("Sheet starts on a new sheet", said_since(mark, "sheet: new /home/o.csv", 30))
        time.sleep(1)
        typed = True
        lines = ["name\tscore\tdouble"] + ["\t".join((n, s, f.format(r=i + 2))) for i, (n, s, f) in enumerate(ROWS)]
        for i, line in enumerate(lines):
            mark = len(vm.serial())
            vm.type(line + "\n")
            typed = typed and said_since(mark, "sheet: A%d " % (i + 2), 20)
        c.add("a header and seven rows typed", typed)

        said = sort_at("B2", "rows 2 to 8 by B, smallest first")
        names, whole = column()
        print("      | by score: %s" % names)
        c.add("by score: numbers smallest first, ties in order, then words, errors, empty last",
              said and names == BY_SCORE)
        c.add("and each row moved whole, its formula its own row's", whole)

        said = sort_at("B2", "rows 2 to 8 by B, largest first")
        names, whole = column()
        print("      | again: %s" % names)
        c.add("ctrl+T again on the column: largest first, errors first, empty still last", said and names == BY_SCORE_DOWN and whole)

        said = sort_at("A2", "rows 2 to 8 by A, smallest first")
        names, whole = column()
        print("      | by name: %s" % names)
        c.add("by name: words with case aside, apple before Banana", said and names == BY_NAME and whole)
        c.add("the header above stays", visit("A1") == "name" and visit("B1") == "score")

        mark = len(vm.serial())
        key("ctrl-s")
        c.add("saved", said_since(mark, "sheet: saved /home/o.csv, 8 rows", 15))
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    try:
        saved = (Fat16(IMAGE).read_file("/home/o.csv") or b"").decode(errors="replace")
    except Exception:
        saved = ""
    back = list(csv.reader(io.StringIO(saved)))
    by = dict((n, (s, f)) for n, s, f in ROWS)
    want = [["name", "score", "double"]] + [[n, by[n][0], by[n][1].format(r=i + 2)] for i, n in enumerate(BY_NAME)]
    if back != want:
        print("      | saved: %r" % saved[:400])
    c.add("the file holds the rows in their last order, formulas moved with them", back == want)

    if not keep:
        try:
            os.remove(IMAGE)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
