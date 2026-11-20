"""The terminal's grep -i and grep -r.

Files of known lines in folders three deep, and a file of bytes with a
nought in it and the word in it too, on a fresh volume. grep is typed into
the desktop's terminal with and without -i and -r, each sent into a file
with >, and the files read back off the disk. A walk's order is the disk's,
so its lines are compared as a set.

  - grep -i finds the word whatever its case; grep alone does not;
  - grep -r finds it in every file below the folder, each line with its
    file's path and number, and passes over the file of bytes;
  - -ri together; -i after a |;
  - an option it does not know is refused, and nothing written.

  python tools/grepcheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "grepcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "grepcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)

FILES = {
    "home/g/a.txt": b"Apple pie\nbanana\n",
    "home/g/sub/b.txt": b"apple tart\nAPPLE juice\ncherry\n",
    "home/g/sub/deep/c.txt": b"no fruit here\npineapple\n",
    "home/g/pic.bin": b"\x89PNG\x00\x01apple\x00\x02\x03\napple again\n",
}
LINES = [
    "grep -i apple /home/g/a.txt > /home/r1.txt",
    "grep apple /home/g/a.txt > /home/r2.txt",
    "grep -r apple /home/g > /home/r3.txt",
    "grep -ri apple /home/g > /home/r4.txt",
    "cat /home/g/sub/b.txt | grep -i apple > /home/r5.txt",
    "grep -x Apple /home/g/a.txt > /home/r6.txt",
    "notice all done",
]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("grep -i and -r")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for i, (path, data) in enumerate(FILES.items()):
        p = os.path.join(WORK, "f%d" % i)
        with open(p, "wb") as f:
            f.write(data)
        args.append(p + ":" + path)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with folders of known lines and a file of bytes", r.returncode == 0)
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
            "grep-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

    got = {n: read("/home/r%d.txt" % n) for n in range(1, 7)}
    for n, text in got.items():
        print("      | r%d: %a" % (n, (text or "")[:200]))
    c.add("grep -i finds Apple", got[1] == "1: Apple pie\n")
    c.add("grep alone does not", got[2] == "")
    walked = {"/home/g/sub/b.txt:1: apple tart", "/home/g/sub/deep/c.txt:2: pineapple"}
    c.add("grep -r finds it in every file below, three deep, with path and number, and not in the bytes",
          got[3] is not None and set(got[3].splitlines()) == walked and len(got[3].splitlines()) == 2)
    both = walked | {"/home/g/a.txt:1: Apple pie", "/home/g/sub/b.txt:2: APPLE juice"}
    c.add("grep -ri finds them all", got[4] is not None and set(got[4].splitlines()) == both
          and len(got[4].splitlines()) == 4)
    c.add("grep -i after a | gives the lines as they are", got[5] == "apple tart\nAPPLE juice\n")
    c.add("an option grep does not know is refused, and nothing written", got[6] == "")

    for i in range(len(FILES)):
        try:
            os.remove(os.path.join(WORK, "f%d" % i))
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
