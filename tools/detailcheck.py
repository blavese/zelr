"""Files says what a folder holds and what a file is.

A folder of files of known sizes, one a folder further down, and a file
beside it, on a fresh volume (mkfat stamps them all 1 January 2025,
00:00). Files is driven by its keys and says on the console what its
status line says.

  - ctrl+P on the folder counts everything below it: its files, its
    folders and their bytes together;
  - on the file, its size and when it was written.

Details in the File menu calls the same function; the key is what is
driven here.

  python tools/detailcheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "detailcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "detailcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}

FILES = {
    "proj/a.txt": b"a" * 1500,
    "proj/sub/b.bin": b"b" * 3000,
    "proj/sub/c.txt": b"c" * 24,
    "one.txt": b"o" * 700,
}


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("details")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for i, (path, data) in enumerate(FILES.items()):
        src = os.path.join(WORK, "f%d" % i)
        with open(src, "wb") as f:
            f.write(data)
        args.append(src + ":home/dt/" + path)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a folder of known sizes and a file beside it", r.returncode == 0)
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
            "detail-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def keys(text):
            for ch in text:
                key(NAMED.get(ch, ch), settle=0.06)

        def said_since(mark, text, timeout=20):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        mark = len(vm.serial())
        keys("/bin/files /home/dt\n")
        c.add("Files opens on the folder", said_since(mark, "files: /home/dt", 30))

        # proj (a folder, so first), then one.txt.
        mark = len(vm.serial())
        key("down")
        said_since(mark, "files: proj - folder", 10)
        mark = len(vm.serial())
        key("ctrl-p")
        # 1500 + 3000 + 24 bytes: 4524, which is 4.4 KB.
        c.add("ctrl+P on the folder counts all below it: 3 files and 1 folder, 4.4 KB",
              said_since(mark, "files: proj: 3 files and 1 folder, 4.4 KB in all"))

        mark = len(vm.serial())
        key("down")
        said_since(mark, "files: one.txt - ", 10)
        mark = len(vm.serial())
        key("ctrl-p")
        c.add("and on a file, its size and when it was written",
              said_since(mark, "files: one.txt: 700 bytes, written 1 January 2025, 00:00"))
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
