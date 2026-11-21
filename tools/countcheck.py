"""Sheet's COUNTIF, SUMIF and AVERAGEIF.

A table of fruit, how many and what they cost typed into a new sheet over
the serial line, then formulas under it; each visited with ctrl+G and what
it says on the console compared with the same worked out here in Python, as
spreadsheets read a criterion.

  - a word equal, with case aside; a word with <>, which a number and an
    empty cell meet too;
  - a number equal, and > >= < <= with numbers, a negative one too, which
    words do not meet;
  - SUMIF and AVERAGEIF of the range itself and of another range beside it;
  - AVERAGEIF that meets nothing (#div/0), a range off the sheet (#ref).

  python tools/countcheck.py [--keep]
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "countcheck.%d.img" % os.getpid())
SEED = os.path.join(ROOT, "countcheck.%d.txt" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)

TABLE = [
    ["apple", "3", "1.5"],
    ["Pear", "-2", "2"],
    ["apple", "7", "0.5"],
    ["fig", "", "4"],
    ["5", "5", "3"],
]
FORMULAS = [
    ['=COUNTIF(A1:A5,"apple")', '=COUNTIF(A1:A5,"<>apple")', '=COUNTIF(B1:B5,5)', '=COUNTIF(B1:B5,">3")'],
    ['=COUNTIF(B1:B5,">=-2")', '=COUNTIF(B1:B5,"<0")', '=SUMIF(A1:A5,"APPLE",B1:B5)', '=SUMIF(B1:B5,">0")'],
    ['=AVERAGEIF(A1:A5,"apple",C1:C5)', '=AVERAGEIF(A1:A5,"kiwi",C1:C5)', '=SUMIF(C1:C5,"<=2")', '=COUNTIF(A1:Z999,1)'],
]


def number(s):
    try:
        return float(s)
    except ValueError:
        return None


def meets(cell, crit):
    """A criterion as spreadsheets read one (and as sheet.c says it does)."""
    if isinstance(crit, (int, float)):
        return number(cell) == crit
    op = "="
    for o in ("<>", "<=", ">=", "<", ">", "="):
        if crit.startswith(o):
            op, crit = o, crit[len(o):]
            break
    want = number(crit)
    have = number(cell) if want is not None else (cell if cell != "" and number(cell) is None else None)
    if have is None:
        return op == "<>"
    a, b = (have, want) if want is not None else (have.lower(), crit.lower())
    return {"=": a == b, "<>": a != b, "<": a < b, ">": a > b, "<=": a <= b, ">=": a >= b}[op]


def col(i):
    return [row[i] for row in TABLE]


def countif(r, crit):
    return sum(1 for v in r if meets(v, crit))


def sumif(r, crit, s=None):
    s = s or r
    return sum(number(s[i]) for i, v in enumerate(r) if meets(v, crit) and number(s[i]) is not None)


def avgif(r, crit, s):
    got = [number(s[i]) for i, v in enumerate(r) if meets(v, crit) and number(s[i]) is not None]
    return "#div/0" if not got else got and sum(got) / len(got)


def shown(x):
    if isinstance(x, str):
        return x
    return ("%.6f" % x).rstrip("0").rstrip(".") if x != int(x) else str(int(x))


A, B, C = col(0), col(1), col(2)
WANT = {
    "A6": shown(countif(A, "apple")), "B6": shown(countif(A, "<>apple")),
    "C6": shown(countif(B, 5)), "D6": shown(countif(B, ">3")),
    "A7": shown(countif(B, ">=-2")), "B7": shown(countif(B, "<0")),
    "C7": shown(sumif(A, "APPLE", B)), "D7": shown(sumif(B, ">0")),
    "A8": shown(avgif(A, "apple", C)), "B8": shown(avgif(A, "kiwi", C)),
    "C8": shown(sumif(C, "<=2")), "D8": "#ref",
}


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("sheet's COUNTIF, SUMIF and AVERAGEIF")
    with open(SEED, "w") as f:
        f.write("seed")
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", SEED + ":home/seed.txt"],
                       cwd=ROOT, capture_output=True, text=True)
    os.remove(SEED)
    c.add("a fresh volume", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)
    print("      | want %a" % WANT)

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "count-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

        mark = len(vm.serial())
        vm.type("/bin/sheet /home/c.csv\n")
        c.add("Sheet starts on a new sheet", said_since(mark, "sheet: new /home/c.csv", 30))
        time.sleep(1)
        rows = TABLE + FORMULAS
        typed = True
        for i, row in enumerate(rows):
            mark = len(vm.serial())
            vm.type("\t".join(row) + "\n")
            typed = typed and said_since(mark, "sheet: A%d " % (i + 2), 20)
        c.add("a table and three rows of formulas typed", typed)

        wrong = []
        for name, value in WANT.items():
            r_, col_ = int(name[1:]) - 1, ord(name[0]) - ord("A")
            want = "%s is %s" % (rows[r_][col_], value)
            got = visit(name)
            if got != want:
                wrong.append("%s %a, not %a" % (name, got, want))
        for w in wrong:
            print("      | " + w)
        groups = {
            "COUNTIF of a word with case aside, and with <>, which numbers and empty cells meet": ["A6", "B6"],
            "COUNTIF of a number, and of > >= < with numbers, a negative one too": ["C6", "D6", "A7", "B7"],
            "SUMIF of another range and of its own, AVERAGEIF of another": ["C7", "D7", "A8", "C8"],
            "AVERAGEIF meeting nothing is #div/0, and a range off the sheet #ref": ["B8", "D8"],
        }
        for what, names in groups.items():
            c.add(what, not any(w.split()[0] in names for w in wrong))
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    if not keep:
        try:
            os.remove(IMAGE)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
