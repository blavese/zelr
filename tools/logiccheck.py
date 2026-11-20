"""Sheet's comparisons, words in formulas, and IF, AND, OR, NOT, ROUND, ABS
and LEN.

Eight rows typed into a new sheet over the serial line, each cell visited
with ctrl+G and what it says on the console compared; a formula with words
in quotes copied and pasted (the words are not references, whatever they
look like); saved, the file read back by Python's csv, and opened again.

  - = <> < > <= >= give TRUE or FALSE: numbers by size, words with case
    aside, numbers below words;
  - & joins words and numbers as they are shown, below + and -;
  - IF with and without its third part, AND, OR, NOT, ROUND to places and
    to hundreds, ABS, LEN;
  - words in quotes, a quote doubled in them; TRUE and FALSE; #value for
    words taken as a truth, #error for a comparison with nothing after it.

  python tools/logiccheck.py [--keep]
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
IMAGE = os.path.join(ROOT, "logiccheck.%d.img" % os.getpid())
SEED = os.path.join(ROOT, "logiccheck.%d.txt" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)

TYPED = [
    ['7', '=A1>5', '=A1<=5', '=IF(A1>5,"big","small")'],
    ['hello', '=A2&" world"', '=A2&A1', '=LEN(B2)'],
    ['=ROUND(2.345,1)', '=ROUND(1234.5,-2)', '=ABS(-3.5)', '=ROUND(-2.5,0)'],
    ['=AND(A1>1,A1<10)', '=OR(A1=1,A1=2)', '=NOT(B1)', '=IF(A1,"yes")'],
    ['=A2="HELLO"', '=A1<>7', '=A2<"world"', '=1<"a"'],
    ['=IF(A1>5,B9,1/0)', '="say ""hi"""', '=SUM(A1,B1)', '=1&2'],
    ['=IF("x",1,2)', '=TRUE+TRUE', '=1+2&3', '=IF(A1<0,"neg")'],
    ['=A1>', '=ROUND(1/3,3)', '=A1+1>B1*7', '="A1"&B8'],
]

WANT = {
    "B1": "TRUE", "C1": "FALSE", "D1": "big",
    "B2": "hello world", "C2": "hello7", "D2": "11",
    "A3": "2.3", "B3": "1200", "C3": "3.5", "D3": "-3",
    "A4": "TRUE", "B4": "FALSE", "C4": "FALSE", "D4": "yes",
    "A5": "TRUE", "B5": "FALSE", "C5": "TRUE", "D5": "TRUE",
    "A6": "0", "B6": 'say "hi"', "C6": "8", "D6": "12",
    "A7": "#value", "B7": "2", "C7": "33", "D7": "FALSE",
    "A8": "#error", "B8": "0.333", "C8": "TRUE", "D8": "A10.333",
}


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("sheet's logic and words")
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
            "logic-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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
        vm.type("/bin/sheet /home/l.csv\n")
        c.add("Sheet starts on a new sheet", said_since(mark, "sheet: new /home/l.csv", 30))
        time.sleep(1)
        typed = True
        for i, row in enumerate(TYPED):
            mark = len(vm.serial())
            vm.type("\t".join(row) + "\n")
            typed = typed and said_since(mark, "sheet: A%d empty" % (i + 2), 20)
        c.add("eight rows of formulas typed", typed)

        wrong = []
        for name, value in WANT.items():
            r_, col = int(name[1:]) - 1, ord(name[0]) - ord("A")
            want = "%s is %s" % (TYPED[r_][col], value) if TYPED[r_][col].startswith("=") else value
            got = visit(name)
            if got != want:
                wrong.append("%s %r, not %r" % (name, got, want))
        for w in wrong:
            print("      | " + w)
        groups = {
            "comparisons give TRUE or FALSE, words with case aside, numbers below words":
                ["B1", "C1", "A5", "B5", "C5", "D5", "C8"],
            "& joins words and numbers as shown, below + and -": ["B2", "C2", "D6", "C7", "D8"],
            "IF, AND, OR, NOT": ["D1", "A4", "B4", "C4", "D4", "A6", "D7"],
            "ROUND to places and to hundreds, half away from nought; ABS; LEN": ["A3", "B3", "C3", "D3", "B8", "D2"],
            "words in quotes with a doubled quote, TRUE and FALSE, #value and #error":
                ["B6", "B7", "A7", "A8", "C6"],
        }
        for what, names in groups.items():
            c.add(what, not any(w.split()[0] in names for w in wrong))

        visit("D8")
        key("ctrl-c")
        visit("D9")
        mark = len(vm.serial())
        key("ctrl-v")
        c.add('a paste moves B8 to B9 and leaves "A1" in quotes alone',
              said_since(mark, 'sheet: D9 ="A1"&B9 is A1', 10))

        mark = len(vm.serial())
        key("ctrl-s")
        c.add("saved", said_since(mark, "sheet: saved /home/l.csv, 9 rows", 15))
        mark = len(vm.serial())
        key("ctrl-o")
        time.sleep(0.3)
        key("ctrl-x")
        vm.type("/home/l.csv\n")
        opened = said_since(mark, "sheet: opened /home/l.csv, 9 rows", 15)
        c.add("opened again, D1 still says big and B6 its quote",
              opened and visit("D1") == '=IF(A1>5,"big","small") is big' and visit("B6") == '="say ""hi""" is say "hi"')
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    try:
        saved = (Fat16(IMAGE).read_file("/home/l.csv") or b"").decode(errors="replace")
    except Exception:
        saved = ""
    back = list(csv.reader(io.StringIO(saved)))
    want_rows = [list(r) for r in TYPED] + [["", "", "", '="A1"&B9']]
    if back != want_rows:
        print("      | saved: %r" % saved[:300])
    c.add("Python's csv reads the file back as typed, quotes and commas in formulas and all", back == want_rows)

    if not keep:
        try:
            os.remove(IMAGE)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
