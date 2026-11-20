"""Files: several entries chosen at once (Insert, ctrl+A), and copied, cut,
pasted, deleted and counted together.

Five files and a folder in /home/m, one file in /home/n, on a fresh
volume. Files is driven by the keyboard and says on the console what its
status line says; what moved is read off the disk image afterwards.

  - Insert marks the entry chosen and moves on: a, b, then d;
  - ctrl+P counts the three marked: 700 bytes;
  - ctrl+C, then ctrl+V in /home/n, copies all three;
  - ctrl+A marks all four there, and Delete moves them all to the trash;
  - in /home/m, ctrl+A and Insert on the folder leave the five files, which
    ctrl+X and ctrl+V in /home/n move;
  - ctrl+A twice marks none.

  python tools/choosecheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "choosecheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "choosecheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}

SIZES = {"a.txt": 100, "b.txt": 200, "c.txt": 300, "d.txt": 400, "e.txt": 500}
FILES = dict(("home/m/" + n, bytes([ord(n[0])]) * s) for n, s in SIZES.items())
FILES["home/m/sub/keep.txt"] = b"k" * 50
FILES["home/n/x.txt"] = b"x" * 10


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("choosing several in files")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for i, (path, data) in enumerate(FILES.items()):
        p = os.path.join(WORK, "f%d" % i)
        with open(p, "wb") as f:
            f.write(data)
        args.append(p + ":" + path)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with two folders of files", r.returncode == 0)
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
            "choose-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

        def press(name, want, timeout=20):
            mark = len(vm.serial())
            key(name)
            return said_since(mark, "files: " + want, timeout)

        def choose(name):
            key("end")
            mark = len(vm.serial())
            key("home")
            for _ in range(12):
                if said_since(mark, "files: %s - " % name, 1.5):
                    return True
                key("down")
            return False

        def go_into(name, path):
            press("backspace", "/home", 10)
            choose(name)
            mark = len(vm.serial())
            key("ret")
            return said_since(mark, "files: " + path + "\r", 10) or said_since(mark, "files: " + path + "\n", 1)

        mark = len(vm.serial())
        for ch in "/bin/files /home/m\n":
            key(NAMED.get(ch, ch), settle=0.06)
        c.add("Files opens on /home/m", said_since(mark, "files: /home/m", 30))

        choose("a.txt")
        marked = press("insert", "1 chosen") and press("insert", "2 chosen")
        key("down")
        marked = marked and press("insert", "3 chosen")
        c.add("Insert marks a, b and then d, moving on each time", marked)
        c.add("ctrl+P counts the three: 700 bytes",
              press("ctrl-p", "3 chosen: 3 files and 0 folders, 700 bytes in all"))
        c.add("ctrl+C takes all three", press("ctrl-c", "copied 3"))

        go_into("n", "/home/n")
        c.add("ctrl+V in /home/n copies all three there", press("ctrl-v", "copied 3 here", 30))
        c.add("ctrl+A there marks all four", press("ctrl-a", "4 chosen"))
        c.add("Delete moves all four to the trash", press("delete", "moved 4 to the trash", 30))

        go_into("m", "/home/m")
        all6 = press("ctrl-a", "6 chosen")
        key("home")
        c.add("ctrl+A marks six, and Insert on the folder leaves five", all6 and press("insert", "5 chosen"))
        c.add("ctrl+P counts the five files: 1.4 KB",
              press("ctrl-p", "5 chosen: 5 files and 0 folders, 1.4 KB in all"))
        c.add("ctrl+X takes the five", press("ctrl-x", "cut 5"))
        go_into("n", "/home/n")
        c.add("ctrl+V in /home/n moves all five there", press("ctrl-v", "moved 5 here", 30))
        c.add("ctrl+A twice marks none", press("ctrl-a", "5 chosen") and press("ctrl-a", "none chosen"))
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    def read(path):
        try:
            return Fat16(IMAGE).read_file(path)
        except Exception:
            return None

    moved = all(read("/home/n/" + n) == FILES["home/m/" + n] for n in SIZES)
    gone = all(read("/home/m/" + n) is None for n in SIZES) and read("/home/m/sub/keep.txt") == b"k" * 50
    c.add("the five are in /home/n and gone from /home/m, the folder's file left", moved and gone)
    trashed = all(read("/home/trash/" + n) is not None for n in ("x.txt", "a.txt", "b.txt", "d.txt"))
    c.add("the four deleted are in the trash", trashed and read("/home/n/x.txt") is None)

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
