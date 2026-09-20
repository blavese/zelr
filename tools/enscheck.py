"""The same two notes, out of the other sound card.

VMware gives a guest whose kind it does not recognise an Ensoniq AudioPCI,
and it does not recognise a system written from scratch. So the controller
zelr is most likely to meet on somebody's own laptop is not the HD Audio one
tools/soundcheck.py exercises, and a driver for it that is never measured is
a driver that plays silence convincingly.

QEMU emulates the ES1370, which is the same chip as VMware's ES1371 with a
different front end: the buffer, the size, the format, the enable bit and the
position are the same registers in the same places, and they are the whole of
what makes a sound. So this measures the shared half properly -- by listening
to it -- and what it cannot reach is the ES1371's rate converter and its AC97
mixer, which are the only two things the other half does.

  python tools/enscheck.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT              # noqa: E402
from soundcheck import (read_wav, loud_runs, frequency_of,       # noqa: E402
                        close_to, LOW_HZ, HIGH_HZ, NOTE_MS)

DISK = os.path.join(ROOT, "enscheck.%d.img" % os.getpid())
WAV = os.path.join(ROOT, "enscheck.%d.wav" % os.getpid())

AUDIO = ["-audiodev", "wav,id=a0,path=" + WAV.replace("\\", "/"),
         "-device", "ES1370,audiodev=a0"]


def main():
    build_once()
    c = Checks("sound on the ensoniq")

    for stale in (WAV, DISK):
        if os.path.exists(stale):
            os.remove(stale)

    vm = Guest(DISK, memory=256, extra=AUDIO)
    try:
        vm.wait_boot()
        boot = vm.serial()
        c.add("an ensoniq audiopci is found and driven",
              "ensoniq es137" in boot)

        # It divides a crystal rather than converting, so it cannot make
        # 48000 and says what it made instead. Anything that produces a note
        # reads that number, which is the whole reason it is reported.
        c.add("and it says what rate it actually agreed to",
              "44100 Hz" in boot)

        out = vm.run("beep %d %d" % (LOW_HZ, NOTE_MS), timeout=40)
        c.add("a note is played without complaint", "done" in out)

        vm.run("beep %d %d" % (HIGH_HZ, NOTE_MS), timeout=40)

        vm.run("uname", timeout=30)
        time.sleep(1.5)
    finally:
        try:
            vm.monitor().send("quit", settle=0.5)
        except Exception:
            pass
        time.sleep(1.0)
        vm.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass

    if not os.path.exists(WAV):
        c.add("something was recorded", False)
        return c.report()

    rate, samples = read_wav(WAV)
    c.add("something was recorded", len(samples) > 0)

    runs = loud_runs(samples, rate)
    if len(runs) != 2:
        print("      heard %d run(s) in %.2fs: %s"
              % (len(runs), len(samples) / float(rate or 1),
                 ", ".join("%.2f-%.2fs" % (a / float(rate), b / float(rate))
                           for a, b in runs)))
    c.add("two notes can be heard, and only two", len(runs) == 2)

    if len(runs) == 2:
        low = frequency_of(samples[runs[0][0]:runs[0][1]], rate)
        high = frequency_of(samples[runs[1][0]:runs[1][1]], rate)

        c.add("the first is the pitch it was asked for (%d Hz, heard %d)"
              % (LOW_HZ, int(low)), close_to(low, LOW_HZ))
        c.add("and the second is the other one (%d Hz, heard %d)"
              % (HIGH_HZ, int(high)), close_to(high, HIGH_HZ))
        c.add("the two are not the same note", abs(high - low) > LOW_HZ / 2.0)

        played = (runs[1][1] - runs[0][0]) / float(rate)
        c.add("and neither note is still playing afterwards",
              runs[1][1] < len(samples) - rate // 4 and played < 4.0)

    try:
        os.remove(WAV)
    except OSError:
        pass
    return c.report()


if __name__ == "__main__":
    sys.exit(main())
