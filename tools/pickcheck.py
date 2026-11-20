"""The file chooser: Notes', Sheet's and Pictures' ctrl+O.

Three files in /home/docs, one of them a folder down, on a fresh volume.
Notes is started on the first from the terminal; the chooser says what it
shows on the console ("notes: choosing DIR, NAME") and what comes of it
("notes: chose PATH"), and Notes says "opened" and "saved". Each file
opened is changed by a letter and saved, so the disk says which was open.

  - the list has the folder first, then the files by name;
  - Down and Return open the file chosen;
  - Return on the folder goes into it, and a file there opens;
  - Backspace goes up, the folder come from chosen, and Escape lets the
    list go, the file open staying open;
  - with changes not saved, ctrl+O says so first, and opens the list on a
    second ctrl+O;
  - a path typed and Return makes a new file there (0.220.0), and a name's
    first letter typed chooses it;
  - Sheet's list has its folder's .csv files and nothing else, and one
    chosen opens;
  - Pictures' ctrl+O opens another picture, and ctrl+O again lets the list
    go (the desktop keeps Escape from Pictures).

  python tools/pickcheck.py [--keep]
"""
import os
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "pickcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "pickcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)


def png(w, h, rgb):
    rows = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


FILES = {"home/docs/a.txt": b"alpha\n", "home/docs/b.txt": b"bravo\n", "home/docs/sub/c.txt": b"charlie\n",
         "home/s/one.csv": b"1,2\n", "home/s/two.csv": b"=1+2\n", "home/s/notes.txt": b"not a sheet\n",
         "home/p/a.png": png(40, 30, (200, 40, 40)), "home/p/b.png": png(40, 30, (40, 40, 200))}


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the file chooser")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for i, (path, data) in enumerate(FILES.items()):
        p = os.path.join(WORK, "f%d" % i)
        with open(p, "wb") as f:
            f.write(data)
        args.append(p + ":" + path)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with three files, one a folder down", r.returncode == 0)
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
            "open-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def said_since(mark, text, timeout=15):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        def press(name, want, timeout=15):
            mark = len(vm.serial())
            key(name)
            return said_since(mark, "notes: " + want, timeout)

        def change_and_save(letter):
            key(letter)
            return press("ctrl-s", "saved")

        mark = len(vm.serial())
        vm.type("/bin/notes /home/docs/a.txt\n")
        c.add("Notes opens a.txt", said_since(mark, "notes: opened", 30))
        time.sleep(1)

        c.add("ctrl+O lists the folder first", press("ctrl-o", "choosing /home/docs, sub"))
        key("down")
        moved = press("down", "choosing /home/docs, b.txt")
        mark = len(vm.serial())
        key("ret")
        chose = said_since(mark, "notes: chose /home/docs/b.txt") and said_since(mark, "notes: opened")
        c.add("Down twice and Return open b.txt", moved and chose and change_and_save("shift-x"))

        press("ctrl-o", "choosing /home/docs, sub")
        inside = press("ret", "choosing /home/docs/sub, c.txt")
        mark = len(vm.serial())
        key("ret")
        chose = said_since(mark, "notes: chose /home/docs/sub/c.txt") and said_since(mark, "notes: opened")
        c.add("Return on the folder goes in, and c.txt there opens", inside and chose and change_and_save("shift-y"))

        press("ctrl-o", "choosing /home/docs/sub, c.txt")
        up = press("backspace", "choosing /home/docs, sub")
        mark = len(vm.serial())
        key("esc")
        gone = said_since(mark, "notes: chose nothing")
        c.add("Backspace goes up with the folder chosen, Escape lets the list go, and c.txt stays open",
              up and gone and change_and_save("shift-z"))

        key("shift-w")
        guarded = press("ctrl-o", "not saved: ctrl+S keeps it, ctrl+O again opens another anyway")
        c.add("with a change not saved ctrl+O says so first, and opens the list the second time",
              guarded and press("ctrl-o", "choosing /home/docs/sub, c.txt"))

        # A path typed: a new file there.
        mark = len(vm.serial())
        vm.type("/home/docs/new.txt")
        typed = said_since(mark, "notes: choosing /home/docs/sub, c.txt, typed /home/docs/new.txt")
        mark = len(vm.serial())
        key("ret")
        made = said_since(mark, "notes: chose /home/docs/new.txt") and said_since(mark, "notes: new file")
        c.add("a path typed and Return open a new file there", typed and made and change_and_save("shift-q"))

        # A name's first letter typed chooses it.
        press("ctrl-o", "choosing /home/docs, ")
        jumped = press("b", "choosing /home/docs, b.txt, typed b")
        mark = len(vm.serial())
        key("ret")
        chose = said_since(mark, "notes: chose /home/docs/b.txt") and said_since(mark, "notes: opened b.txt")
        c.add("a letter typed chooses the name it begins, and Return opens it",
              jumped and chose and change_and_save("shift-v"))
        key("alt-q", settle=1.5)

        # Sheet: its folder's .csv files only.
        mark = len(vm.serial())
        vm.type("/bin/sheet /home/s/one.csv\n")
        said_since(mark, "sheet: opened /home/s/one.csv", 30)
        time.sleep(1)
        mark = len(vm.serial())
        key("ctrl-o")
        listed = said_since(mark, "sheet: choosing /home/s, one.csv")
        key("down")
        key("down")
        time.sleep(1)
        out = vm.serial()[mark:]
        csv_only = "choosing /home/s, two.csv" in out and "notes.txt" not in out
        mark = len(vm.serial())
        key("ret")
        opened = said_since(mark, "sheet: chose /home/s/two.csv") and said_since(mark, "sheet: opened /home/s/two.csv")
        c.add("Sheet's list has the .csv files and nothing else, and the one chosen opens",
              listed and csv_only and opened)
        key("alt-q", settle=1.5)

        # Pictures: another picture, and ctrl+O again lets the list go.
        mark = len(vm.serial())
        vm.type("/bin/pictures /home/p/a.png\n")
        said_since(mark, "pictures: a.png", 30)
        time.sleep(1)
        mark = len(vm.serial())
        key("ctrl-o")
        listed = said_since(mark, "pictures: choosing /home/p, a.png")
        key("down")
        mark = len(vm.serial())
        key("ret")
        shown = said_since(mark, "pictures: chose /home/p/b.png") and said_since(mark, "pictures: b.png, 40 by 30")
        key("ctrl-o")
        mark = len(vm.serial())
        key("ctrl-o")
        let_go = said_since(mark, "pictures: chose nothing")
        c.add("Pictures' ctrl+O shows another picture, and ctrl+O again lets the list go", listed and shown and let_go)
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    def read(path):
        try:
            return Fat16(IMAGE).read_file(path)
        except Exception:
            return None

    got = [read("/home/docs/a.txt"), read("/home/docs/b.txt"), read("/home/docs/sub/c.txt"),
           read("/home/docs/new.txt")]
    print("      | %a" % got)
    c.add("the disk says which was open when: a as it was, b, c and the new file changed and saved",
          got == [b"alpha\n", b"VXbravo\n", b"YZcharlie\n", b"Q"])

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
