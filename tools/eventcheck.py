"""The calendar's times: a part of today's note that begins with a time is
said and rung by the desktop at that minute.

The machine keeps the host's time here (HOST_CLOCK) and its zone is UTC, so
a minute on the host's clock in UTC is the machine's. /home/calendar is put
on the volume before it boots, with today's note holding a time a couple of
minutes on, one ten minutes on, and a time inside words, and tomorrow's
holding the first time again. The desktop says it on the console as a
notice ("notice: calendar: HH:MM TEXT") and rings a note of 880 Hz, which
is recorded and measured.

  - the part with the coming minute is said in that minute, and once;
  - the later part is not, nor the time inside words, nor tomorrow's;
  - the ring is heard, at its pitch.

  python tools/eventcheck.py [--keep]
"""
import datetime
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT, HOST_CLOCK   # noqa: E402
from soundcheck import read_wav, loud_runs, frequency_of, close_to         # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
DISK = os.path.join(ROOT, "eventcheck.%d.img" % os.getpid())
SRC = os.path.join(ROOT, "eventcheck.%d.txt" % os.getpid())
REC = os.path.join(ROOT, "eventrec.%d.wav" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
AUDIO = ["-audiodev", "wav,id=a0,path=" + REC.replace("\\", "/"),
         "-device", "intel-hda", "-device", "hda-output,audiodev=a0"]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the calendar's times")
    for junk in (REC, DISK):
        if os.path.exists(junk):
            os.remove(junk)

    now = datetime.datetime.now(datetime.timezone.utc)
    soon = now + datetime.timedelta(minutes=2 if now.second < 30 else 3)
    when = soon.strftime("%H:%M")
    later = (now + datetime.timedelta(minutes=10)).strftime("%H:%M")
    day = soon.strftime("%Y-%m-%d")
    tomorrow = (soon + datetime.timedelta(days=1)).strftime("%Y-%m-%d")
    note = "%s\tlunch; %s call the bank; %s not this one; ring at %s too\n%s\t%s wrong day\n" % (
        day, when, later, when, tomorrow, when)
    with open(SRC, "w", newline="\n") as f:
        f.write(note)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), DISK, "32768", SRC + ":home/calendar"],
                       cwd=ROOT, capture_output=True, text=True)
    os.remove(SRC)
    c.add("a volume with today's note holding a time (%s)" % when, r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    said_at = None
    vm = Guest(DISK, memory=256, machine="q35", reuse=True, keep=True, extra=HOST_CLOCK + AUDIO)
    try:
        vm.wait_boot()
        mark = len(vm.serial())
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "event-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        end = time.time() + 300
        while time.time() < end and said_at is None:
            if "notice: calendar: %s call the bank" % when in vm.serial()[mark:]:
                said_at = datetime.datetime.now(datetime.timezone.utc)
            time.sleep(0.2)
        c.add("the part with the coming minute is said in that minute (%s, said at %s)"
              % (when, said_at.strftime("%H:%M:%S") if said_at else "-"),
              said_at is not None and said_at.strftime("%H:%M") == when)
        time.sleep(3)
        out = vm.serial()[mark:]
        said = out.count("notice: calendar: %s call the bank" % when)
        c.add("said once, not again each second of its minute (%d)" % said, said == 1)
        rung = [l for l in out.splitlines() if "notice: calendar" in l]
        c.add("not the later part, the time inside words, nor tomorrow's (%d rung)" % len(rung),
              not any(w in l for l in rung for w in ("not this one", "ring at", "wrong day")))
    finally:
        vm.stop()

    rate, samples = read_wav(REC) if os.path.exists(REC) else (0, [])
    runs = loud_runs(samples, rate) if samples else []
    heard = frequency_of(samples[runs[-1][0]:runs[-1][1]], rate) if runs else 0
    c.add("the ring is heard, at 880 Hz (heard %d)" % heard, bool(runs) and close_to(heard, 880))
    if not keep:
        for junk in (DISK, REC):
            try:
                os.remove(junk)
            except OSError:
                pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
