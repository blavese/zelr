"""Wildcards in the terminal: * and ? standing for names.

Files in /home/w on a fresh volume, with names whose order and case are
known; lines typed into the desktop's terminal, each sent into a file with >,
and the files read back off the disk and compared with what Python's
fnmatch makes of the same names, case aside and sorted.

  - echo of *.txt with a folder before it, and of ?.md;
  - cd into the folder and *.txt alone, and B* (case aside);
  - cat of every .txt, in order, and wc after it;
  - "*.txt" in quotes left alone, and a word that matches nothing left as
    typed;
  - rm of *.tmp takes those away and nothing else.

  python tools/globcheck.py [--keep]
"""
import fnmatch
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "globcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "globcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
D = "/home/w"

FILES = {"b.txt": "bee\n", "a.txt": "ay\n", "c.md": "sea\n", "d.md": "dee\n", "ab.md": "long\n",
         "x.tmp": "gone\n", "y.tmp": "gone too\n", "keep.txt": "kept\n"}
LINES = [
    "echo %s/*.txt > %s/o1.out" % (D, D),
    "echo %s/?.md > %s/o2.out" % (D, D),
    "cd %s" % D,
    "echo *.txt > o3.out",
    "echo B* > o4.out",
    "cat *.txt > o5.out",
    'echo "*.txt" and zz*q > o6.out',
    "rm *.tmp",
    "notice all done",
]


def names(pattern, also=()):
    # The names on the disk when each line ran: the .out files made before it
    # are there too, and none matches these patterns.
    have = sorted(list(FILES) + list(also))
    return sorted(n for n in have if fnmatch.fnmatch(n.lower(), pattern.lower()))


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("wildcards")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for name, text in FILES.items():
        p = os.path.join(WORK, name)
        with open(p, "w", newline="\n") as f:
            f.write(text)
        args.append(p + ":home/w/" + name)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with files of known names", r.returncode == 0)
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
            "glob-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

    fat = Fat16(IMAGE)

    def read(name):
        try:
            got = fat.read_file(D + "/" + name)
            return None if got is None else got.decode(errors="replace")
        except Exception:
            return None

    def same(label, name, want):
        got = read(name)
        if got != want:
            print("      | %s: got %a, want %a" % (name, got, want))
        c.add(label, got == want)

    same("a folder's *.txt, in order", "o1.out", " ".join(D + "/" + n for n in names("*.txt")) + "\n")
    same("?.md: one character and no more", "o2.out", " ".join(D + "/" + n for n in names("?.md")) + "\n")
    same("*.txt in the folder cd went to", "o3.out", " ".join(names("*.txt")) + "\n")
    same("B* matches b.txt, case aside", "o4.out", " ".join(names("b*")) + "\n")
    same("cat *.txt gives each file in order, each under its name", "o5.out",
         "".join("== %s ==\n%s" % (n, FILES[n]) for n in names("*.txt")))
    same("a quoted wildcard and one matching nothing are left as typed", "o6.out", "*.txt and zz*q\n")
    left = sorted(p.split("/")[-1].lower() for p, _, _, d, _ in fat.walk() if p.lower().startswith(D + "/") and not d)
    want_left = sorted([n for n in FILES if not n.endswith(".tmp")] + ["o%d.out" % i for i in range(1, 7)])
    if left != want_left:
        print("      | left: %a, want %a" % (left, want_left))
    c.add("rm *.tmp takes those away and nothing else", left == want_left)

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
