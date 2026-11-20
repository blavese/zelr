"""The terminal sends a command's output into a file: > and >>.

Lines are typed into the desktop's terminal, and the files they write are
read back off the disk image afterwards. A `notice` last says on the
console that everything before it has run.

  - `echo one > FILE` writes "one", and `echo two >> FILE` adds "two";
  - `ls / > FILE` writes the listing, `help > FILE` the commands;
  - an error is shown, not written: `cat` of a file that is not there
    leaves an empty file;
  - a program from /bin cannot be sent to a file, and no file is made.

  python tools/redirectcheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

DISK = os.path.join(ROOT, "redirectcheck.%d.img" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("output into a file")
    vm = Guest(DISK, memory=256, keep=True)
    done = False
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "redir-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)
        mark = len(vm.serial())
        for line in ("echo one > /home/e.txt", "echo two >> /home/e.txt", "ls / > /home/root.txt",
                     "help > /home/help.txt", "cat /home/nothing > /home/err.txt",
                     "/bin/calc > /home/calc.txt", "notice all done"):
            vm.type(line + "\n")
        end = time.time() + 60
        while time.time() < end and not done:
            done = "notice: all done" in vm.serial()[mark:]
            time.sleep(0.2)
        c.add("the lines all ran", done)
        time.sleep(1)
    finally:
        vm.stop()

    def read(path):
        try:
            return Fat16(DISK).read_file(path)
        except Exception:
            return None

    e = read("/home/e.txt")
    c.add("> writes, >> adds: one then two", e == b"one\ntwo\n")
    root = (read("/home/root.txt") or b"").decode(errors="replace")
    c.add("ls / into a file has the listing", "home" in root and "bin" in root)
    hlp = (read("/home/help.txt") or b"").decode(errors="replace")
    c.add("help into a file has the commands", "notice" in hlp and "alarm" in hlp and "echo" in hlp)
    err = read("/home/err.txt")
    c.add("an error is shown, not written: the file is empty", err == b"")
    c.add("a program from /bin is not sent to a file, and none is made", read("/home/calc.txt") is None)
    if not keep:
        try:
            os.remove(DISK)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
