"""Files sorted by name, by size and by date.

A folder of files of known sizes on a fresh volume, all stamped with the one
date mkfat writes, and one written from the terminal, which is stamped now.
Files is driven by its keys and says on the console what its status line
says, a line for each entry the selection reaches ("files: NAME - kind"), so
the order is read by walking the list from the top.

  - ctrl+2 sorts by size, the largest first, the folder still first;
  - ctrl+3 by date, the newest first (the one written today), the rest of
    one date by name;
  - ctrl+1 by name again;
  - and the entry chosen stays chosen.

  python tools/sortcheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "sortcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "sortcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}

FILES = {
    "aaa.txt": b"a" * 2000,
    "big.bin": b"b" * 30000,
    "mid.txt": b"m" * 5000,
    "small.txt": b"s" * 100,
    "zdir/in.txt": b"in a folder\n",
}
BY_SIZE = ["zdir", "big.bin", "mid.txt", "aaa.txt", "small.txt", "fresh.txt"]
BY_DATE = ["zdir", "fresh.txt", "aaa.txt", "big.bin", "mid.txt", "small.txt"]
BY_NAME = ["zdir", "aaa.txt", "big.bin", "fresh.txt", "mid.txt", "small.txt"]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("files sorted")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for i, (path, data) in enumerate(FILES.items()):
        src = os.path.join(WORK, "f%d" % i)
        with open(src, "wb") as f:
            f.write(data)
        args.append(src + ":home/srt/" + path)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a folder of files of known sizes", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.run("write /home/srt/fresh.txt written today")
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "sort-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def keys(text):
            for ch in text:
                key(NAMED.get(ch, ch), settle=0.06)

        def said_since(mark, text, timeout=30):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        def order():
            """The list from the top, by walking it."""
            mark = len(vm.serial())
            key("home", settle=0.4)
            for _ in range(len(BY_NAME) - 1):
                key("down", settle=0.4)
            time.sleep(0.5)
            got = []
            for line in vm.serial()[mark:].splitlines():
                if line.startswith("files: ") and " - " in line:
                    name = line[len("files: "):].split(" - ")[0]
                    if name not in got:
                        got.append(name)
            return got

        mark = len(vm.serial())
        keys("/bin/files /home/srt\n")
        c.add("Files opens on the folder", said_since(mark, "files: /home/srt"))
        time.sleep(1)

        mark = len(vm.serial())
        key("ctrl-2")
        said = said_since(mark, "files: sorted by size, the largest first", 10)
        got = order()
        if got != BY_SIZE:
            print("      | by size: %s" % got)
        c.add("ctrl+2 sorts by size, the largest first, the folder first still", said and got == BY_SIZE)

        mark = len(vm.serial())
        key("ctrl-3")
        said = said_since(mark, "files: sorted by date, the newest first", 10)
        got = order()
        if got != BY_DATE:
            print("      | by date: %s" % got)
        c.add("ctrl+3 by date, the one written today first, the rest of one date by name",
              said and got == BY_DATE)

        # small.txt is chosen now (the walk ended there); it stays chosen.
        mark = len(vm.serial())
        key("ctrl-1")
        said = said_since(mark, "files: sorted by name", 10)
        mark = len(vm.serial())
        key("up")
        c.add("ctrl+1 by name again, and the one chosen stays chosen",
              said and said_since(mark, "files: mid.txt - ", 10))
        got = order()
        if got != BY_NAME:
            print("      | by name: %s" % got)
        c.add("in name order", got == BY_NAME)
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

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
