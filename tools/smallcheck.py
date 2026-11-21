"""The terminal's small tools: seq, nl, tac, rev, basename, dirname, touch.

Two files on a fresh volume, one of lines with an empty one among them and
one with mkfat's fixed date on it. The commands are typed into the
desktop's terminal, each sent into a file with >, and the files read back
off the disk and compared with what the same tools give, written out here.
The machine keeps the host's clock (HOST_CLOCK), so touch's date can be
compared with today's.

  - seq LAST, FIRST LAST, FIRST STEP LAST, counting down, below nought;
  - nl, tac after a |, rev, an empty line kept in each;
  - basename with and without an ending and of a path ending in /;
    dirname of a path, of a name alone and of one at the top;
  - touch: a file there written today and as it was, one not there made.

  python tools/smallcheck.py [--keep]
"""
import datetime
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT, HOST_CLOCK   # noqa: E402
from readfat import Fat16                                                 # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "smallcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "smallcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)

LINES_TXT = "one\ntwo\n\nfour\n"
FILES = {"lines.txt": LINES_TXT, "old.txt": "kept\n"}
D = "/home/t/"
OUTS = [
    ("seq 5", "s1", "".join("%d\n" % i for i in range(1, 6))),
    ("seq 3 7", "s2", "".join("%d\n" % i for i in range(3, 8))),
    ("seq 10 -3 1", "s3", "".join("%d\n" % i for i in range(10, 0, -3))),
    ("seq 3 1", "s4", "3\n2\n1\n"),
    ("seq -2 0", "s5", "-2\n-1\n0\n"),
    ("nl %slines.txt" % D, "n1", "".join("%6d\t%s\n" % (i + 1, l) for i, l in enumerate(LINES_TXT.splitlines()))),
    ("cat %slines.txt | tac" % D, "t1", "".join(l + "\n" for l in reversed(LINES_TXT.splitlines()))),
    ("rev %slines.txt" % D, "r1", "".join(l[::-1] + "\n" for l in LINES_TXT.splitlines())),
    ("basename /home/a/b.txt .txt", "b1", "b\n"),
    ("basename /home/a/", "b2", "a\n"),
    ("basename b.txt", "b3", "b.txt\n"),
    ("dirname /home/a/b.txt", "d1", "/home/a\n"),
    ("dirname b.txt", "d2", ".\n"),
    ("dirname /x", "d3", "/\n"),
]
LINES = ["%s > %s%s.txt" % (cmd, D, name) for cmd, name, _ in OUTS] + [
    "touch %sold.txt %snew.txt" % (D, D),
    "notice all done",
]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("small tools")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for name, text in FILES.items():
        p = os.path.join(WORK, name)
        with open(p, "w", newline="\n") as f:
            f.write(text)
        args.append(p + ":home/t/" + name)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with two files", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)
    old_written = Fat16(IMAGE).times(D + "old.txt")[0]

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True, extra=HOST_CLOCK)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "small-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)
        mark = len(vm.serial())
        for line in LINES:
            vm.type(line + "\n")
        end = time.time() + 180
        done = False
        while time.time() < end and not done:
            done = "notice: all done" in vm.serial()[mark:]
            time.sleep(0.2)
        c.add("the lines all ran", done)
        time.sleep(1)
    finally:
        vm.stop()

    fat = Fat16(IMAGE)

    def read(path):
        try:
            got = fat.read_file(path)
            return None if got is None else got.decode(errors="replace")
        except Exception:
            return None

    wrong = []
    for cmd, name, want in OUTS:
        got = read(D + name + ".txt")
        if got != want:
            wrong.append(name)
            print("      | %s: got %a, want %a" % (cmd, got, want))
    groups = {
        "seq up, by a step, down, and below nought": ["s1", "s2", "s3", "s4", "s5"],
        "nl, tac after a | and rev, the empty line kept": ["n1", "t1", "r1"],
        "basename with an ending, of a folder's path, of a name alone": ["b1", "b2", "b3"],
        "dirname of a path, of a name alone and of one at the top": ["d1", "d2", "d3"],
    }
    for what, names in groups.items():
        c.add(what, not any(n in wrong for n in names))

    written = fat.times(D + "old.txt")[0]
    today = datetime.datetime.utcnow()
    print("      | old.txt written %s before, %s after; the host says %s" % (old_written, written, today))
    c.add("touch writes a file that is there again, today, and leaves it as it was",
          written is not None and written != old_written and abs((written - today).total_seconds()) < 2 * 86400
          and read(D + "old.txt") == "kept\n")
    c.add("and makes one that is not, empty", read(D + "new.txt") == "")

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
