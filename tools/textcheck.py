"""Sheet's functions for words: UPPER, LOWER, TRIM, LEFT, RIGHT, MID, FIND.

Six rows typed into a new sheet over the serial line, each cell visited
with ctrl+G and what it says on the console compared with what the same
function gives, worked out here in Python as spreadsheets define it.

  - UPPER, LOWER and TRIM of words with spaces at the ends and inside;
  - LEFT and RIGHT with a count and without, MID inside and past the end;
  - FIND with case counting, from a place, and of words not there;
  - a number taken as shown, an error passed on, a count below nought;
  - the functions inside one another and joined with &.

  python tools/textcheck.py [--keep]
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "textcheck.%d.img" % os.getpid())
SEED = os.path.join(ROOT, "textcheck.%d.txt" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)

PADDED = "  Hello   World  "
WORD = "spreadsheet"
TYPED = [
    ['="%s"' % PADDED, '="["&UPPER(A1)&"]"', '="["&LOWER(A1)&"]"', '="["&TRIM(A1)&"]"'],
    [WORD, '=LEFT(A2,6)', '=RIGHT(A2,5)', '=MID(A2,3,4)'],
    ['=LEFT(A2)', '=RIGHT(A2)', '="["&MID(A2,20,3)&"]"', '=MID(A2,0,2)'],
    ['=FIND("sheet",A2)', '=FIND("S",A2)', '=FIND("e",A2,6)', '=FIND("x",A2)'],
    ['=UPPER(12.5)', '=LEN(TRIM(A1))', '=LEFT(A2,-1)', '=LOWER(1/0)'],
    ['=TRIM(A1)&"!"', '=LEFT(UPPER(A2),3)&RIGHT(A2,2)', '=MID("abc",2,10)', '=LEFT(A2,100)'],
]


def find(needle, hay, start=1):
    at = hay.find(needle, start - 1)
    return "#value" if at < 0 else str(at + 1)


TRIMMED = " ".join(PADDED.split())
WANT = {
    "B1": "[%s]" % PADDED.upper(), "C1": "[%s]" % PADDED.lower(), "D1": "[%s]" % TRIMMED,
    "B2": WORD[:6], "C2": WORD[-5:], "D2": WORD[2:6],
    "A3": WORD[:1], "B3": WORD[-1:], "C3": "[]", "D3": "#value",
    "A4": find("sheet", WORD), "B4": find("S", WORD), "C4": find("e", WORD, 6), "D4": find("x", WORD),
    "A5": "12.5", "B5": str(len(TRIMMED)), "C5": "#value", "D5": "#div/0",
    "A6": TRIMMED + "!", "B6": WORD.upper()[:3] + WORD[-2:], "C6": "bc", "D6": WORD,
}


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("sheet's words")
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
            "text-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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
        vm.type("/bin/sheet /home/w.csv\n")
        c.add("Sheet starts on a new sheet", said_since(mark, "sheet: new /home/w.csv", 30))
        time.sleep(1)
        typed = True
        for i, row in enumerate(TYPED):
            mark = len(vm.serial())
            vm.type("\t".join(row) + "\n")
            typed = typed and said_since(mark, "sheet: A%d empty" % (i + 2), 20)
        c.add("six rows of formulas typed", typed)

        wrong = []
        for name, value in WANT.items():
            r_, col = int(name[1:]) - 1, ord(name[0]) - ord("A")
            want = "%s is %s" % (TYPED[r_][col], value) if TYPED[r_][col].startswith("=") else value
            got = visit(name)
            if got != want:
                wrong.append("%s %a, not %a" % (name, got, want))
        for w in wrong:
            print("      | " + w)
        groups = {
            "UPPER, LOWER and TRIM": ["B1", "C1", "D1", "B5"],
            "LEFT and RIGHT with a count and without": ["B2", "C2", "A3", "B3", "D6"],
            "MID inside, past the end, and from a place below 1": ["D2", "C3", "D3", "C6"],
            "FIND with case counting, from a place, and of words not there": ["A4", "B4", "C4", "D4"],
            "a number as shown, an error passed on, a count below nought, one inside another":
                ["A5", "C5", "D5", "A6", "B6"],
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
