"""Plays notes and listens to what came out.

Every other way of testing audio is a person putting their head next to a
machine. QEMU will write what the guest played to a file instead, so this
boots with an HD Audio controller attached, asks the shell for two notes at
known pitches, and then measures the recording.

That last part is what makes it worth anything. A driver can find the
controller, walk the codec, set up a stream and start it, and produce silence,
and every one of those steps will report success. The only thing that cannot
be faked is a waveform at the frequency that was asked for.

  python tools/soundcheck.py
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "soundcheck.%d.img" % os.getpid())
WAV = os.path.join(ROOT, "soundcheck.%d.wav" % os.getpid())

LOW_HZ = 440
HIGH_HZ = 880
NOTE_MS = 600

AUDIO = ["-audiodev", "wav,id=a0,path=" + WAV.replace("\\", "/"),
         "-device", "intel-hda",
         "-device", "hda-output,audiodev=a0"]


def read_wav(path):
    """Mono samples and the rate.

    Parsed by hand rather than with the wave module, which refuses a file
    whose length fields are zero. QEMU writes those on the way out, and a
    recording of a machine that was shut down any other way is a perfectly
    good recording with a header that says it is empty.
    """
    raw = open(path, "rb").read()
    if len(raw) < 44 or raw[0:4] != b"RIFF" or raw[8:12] != b"WAVE":
        return 0, []

    rate, channels, width = 0, 1, 2
    data = b""
    at = 12
    while at + 8 <= len(raw):
        tag = raw[at:at + 4]
        size = struct.unpack("<I", raw[at + 4:at + 8])[0]
        body = at + 8

        if tag == b"fmt " and body + 16 <= len(raw):
            channels = struct.unpack("<H", raw[body + 2:body + 4])[0]
            rate = struct.unpack("<I", raw[body + 4:body + 8])[0]
            width = struct.unpack("<H", raw[body + 14:body + 16])[0] // 8
        elif tag == b"data":
            # A size of zero means nobody came back to fill it in, so the
            # rest of the file is the recording.
            data = raw[body:body + size] if size else raw[body:]
            break

        if not size:
            break
        at = body + size + (size & 1)

    if width != 2 or not rate or not data:
        return rate, []

    count = len(data) // 2
    values = struct.unpack("<%dh" % count, data[:count * 2])
    if channels < 2:
        return rate, list(values)

    # One channel of it is enough, and both carry the same thing here.
    return rate, list(values[::channels])


def loud_runs(samples, rate, floor=2000, min_ms=120):
    """Where the recording is not silence.

    Measured in blocks rather than sample by sample, because a sine crosses
    zero constantly and a run found by looking at single samples is a run of
    one. A block is loud if anything in it is.
    """
    block = max(1, rate // 200)          # five milliseconds
    loud = []
    for start in range(0, len(samples) - block, block):
        peak = 0
        for s in samples[start:start + block]:
            a = s if s >= 0 else -s
            if a > peak:
                peak = a
        loud.append(peak >= floor)

    runs = []
    at = 0
    while at < len(loud):
        if not loud[at]:
            at += 1
            continue
        end = at
        while end < len(loud) and loud[end]:
            end += 1
        if (end - at) * block >= (rate * min_ms) // 1000:
            runs.append((at * block, end * block))
        at = end
    return runs


def frequency_of(samples, rate):
    """Cycles per second, by counting how often it crosses zero.

    With hysteresis, so that a sample sitting near zero does not read as a
    dozen crossings. Half the crossings are downward, hence the two.

    Measured across the middle of the note rather than all of it. A note now
    fades in and out over five milliseconds at each end, so that it does not
    begin and end with a click, and those ends are below the hysteresis gate
    while still being long enough to count as part of the note: their time is
    in the divisor and their cycles are not in the numerator, which reads as
    a note a couple of percent flat. The pitch of a note is the pitch of the
    part of it that is sounding.
    """
    if not samples:
        return 0.0
    edge = len(samples) // 8
    if edge and len(samples) > 4 * edge:
        samples = samples[edge:len(samples) - edge]
    gate = max(200, max(abs(s) for s in samples) // 4)

    crossings = 0
    state = 0
    for s in samples:
        if state <= 0 and s > gate:
            state = 1
            crossings += 1
        elif state >= 0 and s < -gate:
            state = -1
            crossings += 1

    seconds = len(samples) / float(rate)
    if seconds <= 0:
        return 0.0
    return crossings / 2.0 / seconds


def close_to(measured, want, tolerance=0.06):
    return abs(measured - want) <= want * tolerance


def main():
    build_once()
    c = Checks("sound test")

    for stale in (WAV, DISK):
        if os.path.exists(stale):
            os.remove(stale)

    vm = Guest(DISK, memory=256, machine="q35", extra=AUDIO)
    boot = ""
    try:
        vm.wait_boot()
        boot = vm.serial()
        c.add("an hd audio controller is found", "sound   hda" in boot)
        c.add("and a route out of the codec", "converter" in boot
              and "into pin" in boot)

        out = vm.run("beep %d %d" % (LOW_HZ, NOTE_MS), timeout=40)
        c.add("a note is played without complaint", "done" in out)

        vm.run("beep %d %d" % (HIGH_HZ, NOTE_MS), timeout=40)

        # Keep recording after the last note, so that a buffer still looping
        # the note round has somewhere to show up.
        vm.run("uname", timeout=30)
        time.sleep(1.5)
    finally:
        # Asked to leave rather than killed, so it gets as far as writing the
        # lengths into the header. The monitor goes away mid-sentence when it
        # obeys, which is not a failure.
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
        # A failing check that only says "no" costs another run to work out
        # what it heard, so it says.
        print("      heard %d run(s) in %.2fs: %s"
              % (len(runs), len(samples) / float(rate),
                 ", ".join("%.2f-%.2fs" % (a / float(rate), b / float(rate))
                           for a, b in runs)))
    c.add("two notes can be heard, and only two", len(runs) == 2)

    if len(runs) == 2:
        first = samples[runs[0][0]:runs[0][1]]
        second = samples[runs[1][0]:runs[1][1]]
        low = frequency_of(first, rate)
        high = frequency_of(second, rate)

        c.add("the first is the pitch it was asked for (%d Hz, heard %d)"
              % (LOW_HZ, int(low)), close_to(low, LOW_HZ))
        c.add("and the second is the other one (%d Hz, heard %d)"
              % (HIGH_HZ, int(high)), close_to(high, HIGH_HZ))

        # The two being different is what rules out a driver that plays one
        # fixed noise whatever it is handed.
        c.add("the two are not the same note", abs(high - low) > LOW_HZ / 2.0)

        # A buffer that plays on a loop repeats whatever was last written to
        # it unless something puts silence back. This is that something.
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
