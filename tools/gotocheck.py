"""Notes goes to a line, and a new line keeps the indent of the one before.

A file of thirty numbered lines, the tenth indented by four spaces. Notes is
driven by its keys and says on the console what its status line says
("notes: line 10 of 31"); what it saved is read back off the disk image.

  - ctrl+G, 10 and Return put the cursor on line 10 (of 31: the empty
    line after the last newline is a line, as the cursor can be there);
  - Return at the end of that line starts the next with the same four
    spaces, so x typed there is "    x" on line 11 of the saved file;
  - a number past the end goes to the last line;
  - letters typed into the number are not taken.

  python tools/gotocheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "gotocheck.%d.img" % os.getpid())
SRC = os.path.join(ROOT, "gotocheck.%d.txt" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
LINES = ["line %d" % i if i != 10 else "    indented ten" for i in range(1, 31)]


def read(path):
    try:
        return Fat16(IMAGE).read_file(path)
    except Exception:
        return None


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("go to a line")
    with open(SRC, "wb") as f:
        f.write(("\n".join(LINES) + "\n").encode())
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", SRC + ":home/code.txt"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with thirty lines in a file", r.returncode == 0)
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
            "goto-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

        mark = len(vm.serial())
        vm.type("/bin/notes /home/code.txt\n")
        c.add("Notes opens the file", said_since(mark, "notes: opened", 40))
        time.sleep(0.5)

        mark = len(vm.serial())
        key("ctrl-g")
        key("1")
        key("0")
        key("ret")
        c.add("ctrl+G, 10 and Return go to line 10 (of 31: the file ends in a newline)", said_since(mark, "notes: line 10 of 31"))

        key("end")
        key("ret")
        key("x")
        mark = len(vm.serial())
        key("ctrl-s")
        saved = said_since(mark, "notes: saved")
        time.sleep(1)
        data = read("/home/code.txt")
        got = data.decode(errors="replace").split("\n") if data else []
        if got[9:12] != ["    indented ten", "    x", "line 11"]:
            print("      | lines 10 to 12: %s" % got[9:12])
        c.add("Return at the end of it starts the next with its four spaces: '    x' on line 11",
              saved and got[9:12] == ["    indented ten", "    x", "line 11"] and got[0] == "line 1")

        mark = len(vm.serial())
        key("ctrl-g")
        for d in "999":
            key(d)
        key("ret")
        c.add("a number past the end goes to the last line", said_since(mark, "notes: line 32 of 32"))

        mark = len(vm.serial())
        key("ctrl-g")
        key("a")
        key("2")
        key("b")
        key("ret")
        c.add("letters typed into the number are not taken", said_since(mark, "notes: line 2 of 32"))
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    for junk in (SRC,) + (() if keep else (IMAGE,)):
        try:
            os.remove(junk)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
