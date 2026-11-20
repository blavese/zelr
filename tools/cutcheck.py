"""The terminal's cut and tr, and words in double quotes.

Files of known lines on a fresh volume; the commands are typed into the
desktop's terminal, each sent into a file with >, and the files read back
off the disk and compared with what the same cut or tr, written out here
in Python, makes of them.

  - cut -d, -f2, -d , -f 1,3, -c 2-4 and, after a |, -f2- of a CSV with an
    empty field and a line with no comma (given whole);
  - cut -f 2 of a file apart by tabs (the default), and -d " " -f 2 of one
    apart by spaces (a quoted space is one word);
  - tr a-z A-Z after a |, tr -d of a file, a SET2 shorter than SET1, and
    tr "\\t" "," for an escape;
  - echo "two  spaces" keeps both spaces and loses the quotes, and grep
    finds two words given in quotes.

  python tools/cutcheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "cutcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "cutcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)

CSV = "name,age,town\nann,31,leeds\nbob,4,york\ncy,,hull\nno commas here\n"
TABS = "a\tb\tc\none\ttwo\nsolo\n"
SPACES = "red green blue\nup down\nalone\n"
WORDS = "a b c\nab c\nb c d\nbc\n"
FILES = {"t.csv": CSV, "tabs.txt": TABS, "sp.txt": SPACES, "words.txt": WORDS}
D = "/home/c/"
LINES = [
    "cut -d, -f2 %st.csv > %sc1.txt" % (D, D),
    "cut -d , -f 1,3 %st.csv > %sc2.txt" % (D, D),
    "cut -c 2-4 %st.csv > %sc3.txt" % (D, D),
    "cat %st.csv | cut -d, -f2- > %sc4.txt" % (D, D),
    "cut -f 2 %stabs.txt > %sc5.txt" % (D, D),
    'cut -d " " -f 2 %ssp.txt > %sc6.txt' % (D, D),
    "cat %st.csv | tr a-z A-Z > %st1.txt" % (D, D),
    "tr -d , %st.csv > %st2.txt" % (D, D),
    "tr abc x %swords.txt > %st3.txt" % (D, D),
    'tr "\\t" "," %stabs.txt > %st4.txt' % (D, D),
    'echo "two  spaces" > %sq1.txt' % D,
    'grep "b c" %swords.txt > %sq2.txt' % (D, D),
    "notice all done",
]


def cut_fields(text, delim, wanted):
    out = []
    for line in text.splitlines():
        if delim not in line:
            out.append(line)
            continue
        parts = line.split(delim)
        out.append(delim.join(p for i, p in enumerate(parts) if wanted(i + 1)))
    return "".join(l + "\n" for l in out)


def cut_chars(text, wanted):
    return "".join("".join(ch for i, ch in enumerate(line) if wanted(i + 1)) + "\n" for line in text.splitlines())


def tr(text, a, b):
    return text.translate({ord(x): b[min(i, len(b) - 1)] for i, x in enumerate(a)})


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("cut, tr and quotes")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for name, text in FILES.items():
        p = os.path.join(WORK, name)
        with open(p, "w", newline="\n") as f:
            f.write(text)
        args.append(p + ":home/c/" + name)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with files of known lines", r.returncode == 0)
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
            "cut-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)
        mark = len(vm.serial())
        for line in LINES:
            vm.type(line + "\n")
        end = time.time() + 120
        done = False
        while time.time() < end and not done:
            done = "notice: all done" in vm.serial()[mark:]
            time.sleep(0.2)
        c.add("the lines all ran", done)
        time.sleep(1)
    finally:
        vm.stop()

    def read(name):
        try:
            got = Fat16(IMAGE).read_file(D + name)
            return None if got is None else got.decode(errors="replace")
        except Exception:
            return None

    def same(label, name, want):
        got = read(name)
        if got != want:
            print("      | %s: got %a, want %a" % (name, got, want))
        c.add(label, got == want)

    same("cut -d, -f2: the second field, an empty one empty, a line with no comma whole", "c1.txt",
         cut_fields(CSV, ",", lambda k: k == 2))
    same("cut -d , -f 1,3: two fields, apart by the comma", "c2.txt", cut_fields(CSV, ",", lambda k: k in (1, 3)))
    same("cut -c 2-4: three characters of each line", "c3.txt", cut_chars(CSV, lambda k: 2 <= k <= 4))
    same("cut -d, -f2- after a |: from the second field to the end", "c4.txt", cut_fields(CSV, ",", lambda k: k >= 2))
    same("cut -f 2: fields apart by a tab when no -d is given", "c5.txt", cut_fields(TABS, "\t", lambda k: k == 2))
    same('cut -d " " -f 2: a quoted space is the delimiter', "c6.txt", cut_fields(SPACES, " ", lambda k: k == 2))
    same("tr a-z A-Z after a |", "t1.txt", CSV.upper())
    same("tr -d , takes the commas out", "t2.txt", CSV.replace(",", ""))
    same("tr abc x: a shorter SET2 is made long by its last", "t3.txt", tr(WORDS, "abc", "x"))
    same('tr "\\t" ",": a tab written as \\t', "t4.txt", TABS.replace("\t", ","))
    same('echo "two  spaces": one word, both spaces, no quotes', "q1.txt", "two  spaces\n")
    same('grep "b c" finds the two words together', "q2.txt",
         "".join("%d: %s\n" % (i + 1, l) for i, l in enumerate(WORDS.splitlines()) if "b c" in l))

    for name in FILES:
        try:
            os.remove(os.path.join(WORK, name))
        except OSError:
            pass
    try:
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
