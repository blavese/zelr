"""The terminal's |: one command's output the next one's input.

Files of known lines on a fresh volume. The lines are typed into the
desktop's terminal, each pipe sent into a file with > at its end, and the
files read off the disk image afterwards; a `notice` last says on the
console that the rest ran (a program started from a line would have taken
the keyboard, and it would not).

  - cat | sort, cat | grep | wc, cat | head, cat | sort | tail;
  - sort of a file, and cat | sort | head, of 3000 lines (30000 bytes,
    more than the terminal once read at a time);
  - a | in double quotes is not a pipe;
  - a program from /bin, or a name that is nothing, in a pipe: refused,
    and no file made;
  - grep finding nothing pipes nothing (no "no matches" in the file);
  - cat > FILE copies a file exactly (no empty line added at its end);
  - help | grep finds sort's own line.

  python tools/pipecheck.py [--keep]
"""
import os
import random
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "pipecheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "pipecheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
FRUIT = "pear\napple\nfig\napple\nkiwi\n"
MANY = ["line %04d" % i for i in range(3000)]
random.Random(5).shuffle(MANY)

LINES = [
    "cat /home/p/fruit.txt | calc > /home/calc.txt",
    "cat /home/p/fruit.txt | nosuch > /home/none.txt",
    "cat /home/p/fruit.txt | sort > /home/s.txt",
    "cat /home/p/fruit.txt | grep ap | wc > /home/w.txt",
    "cat /home/p/fruit.txt | head 2 > /home/h.txt",
    "cat /home/p/fruit.txt | sort | tail 1 > /home/t.txt",
    "sort /home/p/many.txt > /home/m.txt",
    "cat /home/p/many.txt | sort | head 3 > /home/mh.txt",
    'echo "a|b" > /home/q.txt',
    "cat /home/p/fruit.txt | grep zz > /home/z.txt",
    "cat /home/p/fruit.txt > /home/copy.txt",
    "help | grep sort > /home/hs.txt",
    "notice all done",
]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("pipes")
    os.makedirs(WORK, exist_ok=True)
    fruit = os.path.join(WORK, "fruit")
    many = os.path.join(WORK, "many")
    with open(fruit, "wb") as f:
        f.write(FRUIT.encode())
    with open(many, "wb") as f:
        f.write(("\n".join(MANY) + "\n").encode())
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768",
                        fruit + ":home/p/fruit.txt", many + ":home/p/many.txt"],
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
            "pipe-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)
        mark = len(vm.serial())
        for line in LINES:
            vm.type(line + "\n")
        end = time.time() + 90
        done = False
        while time.time() < end and not done:
            done = "notice: all done" in vm.serial()[mark:]
            time.sleep(0.2)
        c.add("the lines all ran (no program took the keyboard)", done)
        time.sleep(1)
    finally:
        vm.stop()

    def read(path):
        try:
            got = Fat16(IMAGE).read_file(path)
            return None if got is None else got.decode(errors="replace")
        except Exception:
            return None

    def check(what, path, want):
        got = read(path)
        if got != want:
            print("      | %s: %r" % (path, (got or "")[:200]))
        c.add(what, got == want)

    c.add("a program from /bin in a pipe is refused, and no file made", read("/home/calc.txt") is None)
    c.add("a name that is nothing in a pipe is refused, and no file made", read("/home/none.txt") is None)
    check("cat | sort", "/home/s.txt", "".join(l + "\n" for l in sorted(FRUIT.split())))
    check("cat | grep ap | wc counts the two lines grep gave", "/home/w.txt",
          "%7d lines%8d words%9d bytes\n" % (2, 2, 12))
    check("cat | head 2", "/home/h.txt", "pear\napple\n")
    check("cat | sort | tail 1", "/home/t.txt", "pear\n")
    check("sort of a file of 3000 lines", "/home/m.txt", "".join(l + "\n" for l in sorted(MANY)))
    check("cat | sort | head 3 of 3000 lines", "/home/mh.txt", "".join(l + "\n" for l in sorted(MANY)[:3]))
    check("a | inside double quotes is not a pipe, and the quotes go (0.223.0)", "/home/q.txt", 'a|b\n')
    check("grep finding nothing pipes nothing", "/home/z.txt", "")
    check("cat > FILE copies a file exactly", "/home/copy.txt", FRUIT)
    hs = read("/home/hs.txt") or ""
    c.add("help | grep sort finds sort's own line", any(l.strip().startswith("sort [FILE]") for l in hs.splitlines()))

    for p in (fruit, many):
        try:
            os.remove(p)
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
