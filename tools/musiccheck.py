"""Music from its keys: a list played on, a pause that keeps the place.

Three WAVs in /home, each a tone of its own pitch: A at 440 Hz, B at 550
and C at 660, A and B short and C long. Music is started with no file and
driven by keys only, and what comes out of the machine is recorded:

  - Music lists the three;
  - Down and Return play B, the second: the first sound heard is at 550 Hz,
    not A's 440;
  - when B ends C follows by itself: the same sound ends at 660 Hz;
  - Space pauses C and Space goes on with it: the recording has a gap, and
    B and C together are as long as the files (a pause that dropped what
    the card held, or started C again, would not be);
  - Right plays the next one, from the one chosen.

  python tools/musiccheck.py [--keep]
"""
import math
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT      # noqa: E402
from soundcheck import read_wav, loud_runs, frequency_of, close_to  # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "musiccheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "musiccheck.%d" % os.getpid())
REC = os.path.join(ROOT, "musicrec.%d.wav" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
RATE = 22050
TONES = [("A.WAV", 440, 0.6), ("B.WAV", 550, 0.6), ("C.WAV", 660, 2.4)]

AUDIO = ["-audiodev", "wav,id=a0,path=" + REC.replace("\\", "/"),
         "-device", "intel-hda",
         "-device", "hda-output,audiodev=a0"]


def tone(hz, seconds):
    n = int(RATE * seconds)
    body = b"".join(struct.pack("<h", int(9000 * math.sin(2 * math.pi * hz * i / RATE))) for i in range(n))
    return (b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
            + b"data" + struct.pack("<I", len(body)) + body)


def sounds(samples, rate):
    """Stretches of sound, a run broken by less than half a second taken as
    one (a busy host can leave a gap in a recording), and how much of each
    was sound."""
    groups = []
    for start, end in loud_runs(samples, rate, min_ms=10):
        if groups and start - groups[-1][1] < rate // 2:
            groups[-1][1] = end
            groups[-1][2] += end - start
        else:
            groups.append([start, end, end - start])
    return groups


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("music from its keys")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for name, hz, secs in TONES:
        src = os.path.join(WORK, name)
        with open(src, "wb") as f:
            f.write(tone(hz, secs))
        args.append(src + ":home/" + name)
    if os.path.exists(REC):
        os.remove(REC)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with three tones", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    vm = Guest(IMAGE, memory=256, machine="q35", extra=AUDIO, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "music-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def said_since(mark, text, timeout=30):
            end = time.time() + timeout
            while time.time() < end:
                if text.lower() in vm.serial()[mark:].lower():      # mkfat's names come back in lower case
                    return True
                time.sleep(0.1)
            return False

        mark = len(vm.serial())
        vm.type("music\n")
        c.add("Music lists the three", said_since(mark, "music: 3 files to play"))
        time.sleep(1.5)

        mark = len(vm.serial())
        key("down")
        key("ret")
        c.add("Down and Return play the second", said_since(mark, "music: playing B.WAV"))
        c.add("and the next follows it by itself", said_since(mark, "music: playing C.WAV"))

        # A moment into C, a pause; then on again.
        time.sleep(0.8)
        mark = len(vm.serial())
        key("spc")
        c.add("Space pauses it", said_since(mark, "music: paused C.WAV", 10))
        time.sleep(2.5)
        mark = len(vm.serial())
        key("spc")
        c.add("and Space goes on with it", said_since(mark, "music: going on with C.WAV", 10))
        c.add("to its end", said_since(mark, "music: finished with", 30))
        time.sleep(1.5)

        # Nothing plays now; A is chosen with Home, and Right plays the next.
        mark = len(vm.serial())
        key("home")
        key("right")
        c.add("Right plays the next one from the one chosen", said_since(mark, "music: playing B.WAV", 10))
        time.sleep(0.3)
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    rate, samples = read_wav(REC) if os.path.exists(REC) else (0, [])
    heard = sounds(samples, rate) if samples else []
    print("      | sounds: " + ", ".join("%.2f s at %.2f (%.2f s of sound)" % ((e - s) / float(rate or 1), s / float(rate or 1), n / float(rate or 1))
                                        for s, e, n in heard))
    c.add("B and C, then a gap, then the rest of C (%d sounds before the last)" % (len(heard) - 1), len(heard) >= 3)
    if len(heard) >= 3:
        (s0, e0, n0), (s1, e1, n1) = heard[0], heard[1]
        first = frequency_of(samples[s0:s0 + int(rate * 0.45)], rate)
        last = frequency_of(samples[e1 - int(rate * 0.5):e1], rate)
        c.add("the first sound heard is B's 550 Hz (heard %d)" % first, close_to(first, 550))
        c.add("C ends it at 660 Hz (heard %d)" % last, close_to(last, 660))
        total = (n0 + n1) / float(rate)
        want = TONES[1][2] + TONES[2][2]
        c.add("B and C are as long as the files, the pause losing nothing (%.2f s of %.2f)" % (total, want),
              abs(total - want) <= 0.2)

    for name, _, _ in TONES:
        try:
            os.remove(os.path.join(WORK, name))
        except OSError:
            pass
    try:
        os.rmdir(WORK)
    except OSError:
        pass
    if not keep:
        for junk in (IMAGE, REC):
            try:
                os.remove(junk)
            except OSError:
                pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
