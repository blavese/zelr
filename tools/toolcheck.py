"""The terminal's cal, du and df, their output sent into files and read back.

A folder of files of known sizes on a fresh volume. The commands are typed
into the desktop's terminal with > FILE, and the files are read off the
disk image afterwards; a `notice` last says on the console that the rest
ran. cal is compared with Python's calendar module, whose layout is cal's.

  - cal 2 2028 is February 2028, a leap year's, as cal lays it out;
  - cal with no month is this month;
  - a month that is not one is an error, shown and not written;
  - du counts the folder's bytes, files and folders all the way down;
  - df gives the disk's size, used and free as /sys/storage counts them,
    and says there is no stick.

  python tools/toolcheck.py [--keep]
"""
import calendar
import re
import datetime
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "toolcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "toolcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
FILES = {"proj/a.txt": 1500, "proj/sub/b.bin": 3000, "proj/sub/c.txt": 24, "one.txt": 700}


def cal_text(y, m):
    return "\n".join(l.rstrip() for l in calendar.TextCalendar(0).formatmonth(y, m).rstrip("\n").split("\n")) + "\n"


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("cal, du and df")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for i, (path, size) in enumerate(FILES.items()):
        src = os.path.join(WORK, "f%d" % i)
        with open(src, "wb") as f:
            f.write(b"x" * size)
        args.append(src + ":home/dt/" + path)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a folder of known sizes", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    today = datetime.datetime.now(datetime.timezone.utc)
    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "tool-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)
        mark = len(vm.serial())
        for line in ("cal 2 2028 > /home/cal.txt", "cal > /home/now.txt", "cal 13 2028 > /home/bad.txt",
                     "du /home/dt > /home/du.txt", "cat /sys/storage > /home/st.txt", "df > /home/df.txt",
                     "notice all done"):
            vm.type(line + "\n")
        end = time.time() + 60
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
            return (Fat16(IMAGE).read_file(path) or b"").decode(errors="replace")
        except Exception:
            return None

    got = read("/home/cal.txt")
    if got != cal_text(2028, 2):
        print("      | cal 2 2028:\n" + (got or ""))
    c.add("cal 2 2028 is February of a leap year, laid out as cal is", got == cal_text(2028, 2))
    c.add("cal alone is this month", read("/home/now.txt") == cal_text(today.year, today.month))
    c.add("a month that is not one is shown as an error, not written", read("/home/bad.txt") == "")
    du = read("/home/du.txt")
    c.add("du counts all below the folder: 5224 bytes, 4 files, 2 folders (%s)" % (du or "").strip(),
          du == "5224 bytes in 4 files and 2 folders: /home/dt\n")
    df = read("/home/df.txt") or ""
    st = read("/home/st.txt") or ""
    print("      | df: " + " / ".join(df.splitlines()))
    print("      | /sys/storage: " + " / ".join(st.splitlines()))
    sm = re.search(r"disk +(\d+) KiB, (\d+) KiB free", st)
    dm = re.search(r"^disk: (\d+)\.(\d) MB, (\d+)\.(\d) MB used, (\d+)\.(\d) MB free$", df, re.M)
    fair = False
    if sm and dm:
        total, free = int(sm.group(1)), int(sm.group(2))
        said = [int(dm.group(i)) * 10 + int(dm.group(i + 1)) for i in (1, 3, 5)]
        # The file st.txt is written between the two readings, so free may
        # differ by a cluster or two: a tenth of a megabyte either way.
        fair = (said[0] == total * 10 // 1024 and abs(said[2] - free * 10 // 1024) <= 1
                and abs(said[0] - said[1] - said[2]) <= 1)
    c.add("df gives the disk's size, used and free as /sys/storage counts them", fair)
    c.add("df says there is no stick", "stick: none" in df)

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
