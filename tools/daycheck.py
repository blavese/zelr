"""The desktop says today's calendar note when it opens.

/home/calendar is written onto a fresh volume as the calendar writes it, a
line a day ("2026-11-20<tab>the dentist at ten"), for yesterday, today and
tomorrow by the host's clock in UTC, which is the machine's local time
here (its zone is UTC unless set, and its clock is the host's). The
desktop says a notice on the console as "notice: TEXT".

  - opened, the desktop says today's note, and not another day's;
  - it is among the notices kept (alt+N);
  - opened a second time the same day, it does not say it again.

The date turning over is the self test's to check ([windows], the note
found for a given date); a harness cannot make a day pass.

  python tools/daycheck.py [--keep]
"""
import datetime
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "daycheck.%d.img" % os.getpid())
SRC = os.path.join(ROOT, "daycheck.%d.txt" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("today's note")
    today = datetime.datetime.now(datetime.timezone.utc).date()
    one = datetime.timedelta(days=1)
    with open(SRC, "w", newline="\n") as f:
        f.write("%s\tthe day before\n%s\tthe dentist at ten\n%s\tthe day after\n" % (today - one, today, today + one))
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", SRC + ":home/calendar"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a calendar of three days' notes", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()

        def said_since(mark, text, timeout=30):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        mark = len(vm.serial())
        vm.type("desktop\n")
        said = said_since(mark, "notice: today: the dentist at ten", 60)
        c.add("opened, the desktop says today's note", said)
        out = vm.serial()[mark:]
        c.add("and not another day's", "the day before" not in out and "the day after" not in out)

        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "day-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        mark = len(vm.serial())
        mon.send("sendkey alt-n", settle=0.5)
        c.add("it is among the notices kept", said_since(mark, "desktop: notices: 1: today: the dentist at ten"))
        mon.send("sendkey alt-n", settle=0.5)

        # Left and opened again: the same day, nothing new to say.
        mark = len(vm.serial())
        mon.send("sendkey alt-esc", settle=1.0)
        vm.wait_prompt(vm.prompts() + 1, 30)
        vm.type("desktop\n")
        _, _, _, shot, up = mon.wait_screen(
            "day-again", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        time.sleep(3)
        c.add("opened again the same day, it does not say it again",
              up and "notice: today:" not in vm.serial()[mark:], shot)
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
