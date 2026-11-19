"""Music's repeat and shuffle.

Six short tones in /home, A to F. Music says on the console what it plays
("music: playing NAME, ...") and what R and S set ("music: repeat on,
shuffle off"), so the order is read from there.

  - R and Return on the first: A to F, then A again;
  - R again: the list ends after F;
  - S, with repeat off, twice: started on A, the other five follow, each
    heard once, and then it stops; and not both times in the list's order
    (which a true shuffle does one time in 120, twice one in 14400).

  python tools/shufflecheck.py [--keep]
"""
import math
import os
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "shufflecheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "shufflecheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
RATE = 11025
SONGS = [("A.WAV", 440), ("B.WAV", 495), ("C.WAV", 550), ("D.WAV", 587), ("E.WAV", 660), ("F.WAV", 733)]
ORDER = ["a.wav", "b.wav", "c.wav", "d.wav", "e.wav", "f.wav"]
AUDIO = ["-audiodev", "none,id=a0", "-device", "intel-hda", "-device", "hda-output,audiodev=a0"]


def tone(hz, seconds):
    n = int(RATE * seconds)
    body = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * hz * i / RATE))) for i in range(n))
    return (b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
            + b"data" + struct.pack("<I", len(body)) + body)


def played(text):
    return [m.lower() for m in re.findall(r"music: playing (\w+\.wav)", text, re.I)]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("repeat and shuffle")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for name, hz in SONGS:
        src = os.path.join(WORK, name)
        with open(src, "wb") as f:
            f.write(tone(hz, 0.3))
        args.append(src + ":home/" + name)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with six short tones", r.returncode == 0)
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
            "shuffle-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def wait_for(mark, test, timeout=60):
            end = time.time() + timeout
            while time.time() < end:
                if test(vm.serial()[mark:]):
                    return True
                time.sleep(0.2)
            return False

        mark = len(vm.serial())
        vm.type("music\n")
        wait_for(mark, lambda s: "music: 6 files to play" in s)
        time.sleep(1.5)

        mark = len(vm.serial())
        key("r")
        on = wait_for(mark, lambda s: "music: repeat on, shuffle off" in s, 10)
        key("ret")
        went_round = wait_for(mark, lambda s: played(s)[:7] == ORDER + ["a.wav"])
        if not went_round:
            print("      | played: %s" % played(vm.serial()[mark:]))
        c.add("R and Return on the first: A to F, then A again", on and went_round)

        mark = len(vm.serial())
        key("r")
        off = wait_for(mark, lambda s: "music: repeat off, shuffle off" in s, 10)
        ended = wait_for(mark, lambda s: played(s) and played(s)[-1] == "f.wav" and "music: finished with" in s.split("music: playing f.wav")[-1])
        time.sleep(3)
        after = played(vm.serial()[mark:])
        c.add("R again: the list ends after F", off and ended and after[-1] == "f.wav")

        mark = len(vm.serial())
        key("s")
        on = wait_for(mark, lambda s: "music: repeat off, shuffle on" in s, 10)
        runs = []
        for _ in range(2):
            mark = len(vm.serial())
            key("home")
            key("ret")
            wait_for(mark, lambda s: len(played(s)) >= 6 and "music: finished with" in s.split("music: playing")[-1])
            time.sleep(2)
            runs.append(played(vm.serial()[mark:]))
        print("      | shuffled: %s" % runs)
        each_once = all(len(g) == 6 and g[0] == "a.wav" and sorted(g) == ORDER for g in runs)
        c.add("S: started on A, the other five follow, each once, and then it stops (both times)", on and each_once)
        c.add("and not both times in the list's order", each_once and any(g != ORDER for g in runs))
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    for name, _ in SONGS:
        try:
            os.remove(os.path.join(WORK, name))
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
