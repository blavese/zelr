"""The terminal's diff and uniq.

Files of known lines on a fresh volume; the commands are typed into the
desktop's terminal, each sent into a file with >, and the files read back
off the disk. A diff is judged by what it must be, not by one way of
writing it: its lines, applied to the first file, must give the second,
there must be no more of them than the fewest that can (worked out here
from the longest run of lines the two share), and each must carry its own
file's line number.

  - diff of two short files with lines changed, gone and new;
  - diff of two files of 900 lines with changes scattered through them;
  - diff of a file with itself: nothing in the file (and "the same" only
    on the screen);
  - uniq after a |, uniq -c of a file, and sort | uniq -c.

  python tools/diffcheck.py [--keep]
"""
import os
import random
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "diffcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "diffcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)

A = ["one", "two", "three", "four", "five", "six", "seven", "eight"]
B = ["one", "2", "three", "five", "six", "six and a half", "seven", "eight", "nine"]
rng = random.Random(11)
BIG_A = ["line %d %s" % (i, rng.choice("abcdefgh")) for i in range(900)]
BIG_B = list(BIG_A)
for _ in range(40):
    k = rng.randrange(len(BIG_B))
    op = rng.choice("dic")
    if op == "d":
        del BIG_B[k]
    elif op == "i":
        BIG_B.insert(k, "new %d" % rng.randrange(10000))
    else:
        BIG_B[k] = "changed %d" % rng.randrange(10000)
U = ["pear", "pear", "apple", "apple", "apple", "pear", "fig", "fig", "kiwi"]

FILES = {"a.txt": A, "b.txt": B, "big1.txt": BIG_A, "big2.txt": BIG_B, "u.txt": U}
LINES = [
    "diff /home/d/a.txt /home/d/b.txt > /home/d/small.txt",
    "diff /home/d/big1.txt /home/d/big2.txt > /home/d/big.txt",
    "diff /home/d/a.txt /home/d/a.txt > /home/d/same.txt",
    "cat /home/d/u.txt | uniq > /home/d/u1.txt",
    "uniq -c /home/d/u.txt > /home/d/u2.txt",
    "cat /home/d/u.txt | sort | uniq -c > /home/d/u3.txt",
    "notice all done",
]


def lcs(a, b):
    n, m = len(a), len(b)
    row = [0] * (m + 1)
    for i in range(n - 1, -1, -1):
        new = [0] * (m + 1)
        for j in range(m - 1, -1, -1):
            new[j] = row[j + 1] + 1 if a[i] == b[j] else max(row[j], new[j + 1])
        row = new
    return row[0]


def judge(text, a, b):
    """Whether a diff's lines turn a into b, with the right line numbers,
    and are as few as can be. (good, how many lines, why not)"""
    lines = text.splitlines()
    gone, come = {}, {}
    for line in lines:
        m = re.match(r"([-+]) (\d+): (.*)$", line)
        if not m:
            return False, len(lines), "a line that is not a diff's: %r" % line
        sign, num, body = m.group(1), int(m.group(2)), m.group(3)
        if sign == "-":
            if num < 1 or num > len(a) or a[num - 1] != body:
                return False, len(lines), "- %d is not a's line %d" % (num, num)
            gone[num] = body
        else:
            if num < 1 or num > len(b) or b[num - 1] != body:
                return False, len(lines), "+ %d is not b's line %d" % (num, num)
            come[num] = body
    kept = [line for i, line in enumerate(a) if i + 1 not in gone]
    left = [line for i, line in enumerate(b) if i + 1 not in come]
    if kept != left:
        return False, len(lines), "what is left of a is not what is left of b"
    fewest = len(a) + len(b) - 2 * lcs(a, b)
    if len(lines) != fewest:
        return False, len(lines), "%d lines, and %d would do" % (len(lines), fewest)
    return True, len(lines), ""


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("diff and uniq")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for name, lines in FILES.items():
        p = os.path.join(WORK, name)
        with open(p, "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        args.append(p + ":home/d/" + name)
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
            "diff-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

    def read(path):
        try:
            got = Fat16(IMAGE).read_file(path)
            return None if got is None else got.decode(errors="replace")
        except Exception:
            return None

    ok, n, why = judge(read("/home/d/small.txt") or "", A, B)
    if not ok:
        print("      | small: " + why)
    c.add("diff of two short files: the fewest lines, each with its own number (%d)" % n, ok)
    ok, n, why = judge(read("/home/d/big.txt") or "", BIG_A, BIG_B)
    if not ok:
        print("      | big: " + why)
    c.add("diff of two files of 900 lines with forty changes scattered (%d lines)" % n, ok)
    c.add("diff of a file with itself writes nothing", read("/home/d/same.txt") == "")
    u1 = read("/home/d/u1.txt")
    c.add("uniq after a | says lines alike in a row once", u1 == "pear\napple\npear\nfig\nkiwi\n")
    u2 = read("/home/d/u2.txt")
    c.add("uniq -c counts them", u2 == "".join("%7d %s\n" % (k, w) for k, w in
                                               ((2, "pear"), (3, "apple"), (1, "pear"), (2, "fig"), (1, "kiwi"))))
    u3 = read("/home/d/u3.txt")
    c.add("sort | uniq -c counts every line", u3 == "".join("%7d %s\n" % (U.count(w), w) for w in sorted(set(U))))
    for name, got in (("u1", u1), ("u2", u2), ("u3", u3)):
        print("      | %s: %r" % (name, (got or "")[:120]))

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
