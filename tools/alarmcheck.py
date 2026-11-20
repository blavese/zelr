"""Alarms: set from the terminal, said and rung by the desktop at their minute.

The machine keeps the host's time here (HOST_CLOCK) and its zone is UTC, so
a minute on the host's clock in UTC is the machine's. Two alarms are set
with the terminal's `alarm`, one for the coming minute and one ten minutes
on; the desktop says an alarm on the console as a notice ("notice: alarm:
TEXT") and rings a note of 880 Hz, which is recorded and measured.

  - `alarm HH:MM TEXT` writes it into /home/alarms, read back off the disk;
  - the first is said in its own minute, and once;
  - and the other not;
  - and the ring is heard, at its pitch.

  python tools/alarmcheck.py [--keep]
"""
import datetime
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT, HOST_CLOCK   # noqa: E402
from readfat import Fat16                                                 # noqa: E402
from soundcheck import read_wav, loud_runs, frequency_of, close_to         # noqa: E402

DISK = os.path.join(ROOT, "alarmcheck.%d.img" % os.getpid())
REC = os.path.join(ROOT, "alarmrec.%d.wav" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
AUDIO = ["-audiodev", "wav,id=a0,path=" + REC.replace("\\", "/"),
         "-device", "intel-hda", "-device", "hda-output,audiodev=a0"]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("alarms")
    if os.path.exists(REC):
        os.remove(REC)
    vm = Guest(DISK, memory=256, machine="q35", keep=True, extra=HOST_CLOCK + AUDIO)
    when = said_at = None
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "alarm-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        now = datetime.datetime.now(datetime.timezone.utc)
        when = (now + datetime.timedelta(minutes=2 if now.second > 45 else 1)).strftime("%H:%M")
        later = (now + datetime.timedelta(minutes=10)).strftime("%H:%M")
        mark = len(vm.serial())
        vm.type("alarm %s time to stretch\n" % when)
        vm.type("alarm %s not this one\n" % later)
        end = time.time() + 200
        while time.time() < end and said_at is None:
            if "notice: alarm: time to stretch" in vm.serial()[mark:]:
                said_at = datetime.datetime.now(datetime.timezone.utc)
            time.sleep(0.2)
        c.add("the alarm is said in its own minute (%s, said at %s)"
              % (when, said_at.strftime("%H:%M:%S") if said_at else "-"),
              said_at is not None and said_at.strftime("%H:%M") == when)
        c.add("and the other is not", "not this one" not in vm.serial()[mark:])
        time.sleep(3)
        said = vm.serial()[mark:].count("notice: alarm: time to stretch")
        c.add("said once, not again each second of its minute (%d)" % said, said == 1)
    finally:
        vm.stop()

    data = None
    try:
        data = Fat16(DISK).read_file("/home/alarms")
    except Exception:
        pass
    lines = data.decode(errors="replace").splitlines() if data else []
    c.add("alarm wrote both into /home/alarms", lines == ["%s time to stretch" % when, "%s not this one" % later])

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
