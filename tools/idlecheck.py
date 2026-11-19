"""The screen locks by itself when nobody is there.

lock_after is a setting in minutes, so this takes minutes: it is set to one
from the terminal (written into /zelr.cfg, as Settings writes it), and the
desktop says on the console when it locks ("desktop: locked"). Times here
are the host's, and the machine's clock never runs ahead of the host's
under QEMU (it runs behind on a busy one), so a lock sooner than the minute
on this clock is a lock sooner than the minute.

  - with nothing touched, it locks after a minute, and not before;
  - the pointer moving now and then keeps it open past the minute;
  - sound playing keeps it open while it plays, past the minute, and the
    minute counts from when the sound stopped.

  python tools/idlecheck.py [--keep]
"""
import math
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT      # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "idlecheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "idlecheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
RATE = 8000
SONG_S = 80                      # longer than the minute
AUDIO = ["-audiodev", "none,id=a0", "-device", "intel-hda", "-device", "hda-output,audiodev=a0"]


def long_tone():
    n = RATE * SONG_S
    body = b"".join(struct.pack("<h", int(6000 * math.sin(2 * math.pi * 300 * i / RATE))) for i in range(n))
    return (b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
            + b"data" + struct.pack("<I", len(body)) + body)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("locked when nobody is there")
    os.makedirs(WORK, exist_ok=True)
    song = os.path.join(WORK, "LONG.WAV")
    with open(song, "wb") as f:
        f.write(long_tone())
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", song + ":home/LONG.WAV"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a song longer than a minute", r.returncode == 0)
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
            "idle-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def said_since(mark, text, timeout):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return time.time()
                time.sleep(0.2)
            return None

        # Nothing touched: locked after the minute and not before.
        mark = len(vm.serial())
        vm.type("append /zelr.cfg lock_after 1\n")
        set_at = time.time()
        at = said_since(mark, "desktop: locked", 240)
        took = (at - set_at) if at else -1
        c.add("with nothing touched it locks by itself after the minute (%.0f s)" % took,
              at is not None and took >= 58)

        # The pointer moving every fifteen seconds keeps it open past the minute.
        mark = len(vm.serial())
        mon.send("sendkey ret", settle=0.5)
        c.add("Return opens it (no password)", said_since(mark, "desktop: unlocked", 20) is not None)
        mark = len(vm.serial())
        for i in range(6):
            time.sleep(15)
            mon.move_to(600 + (i % 2) * 40, 300)
        c.add("the pointer moving now and then keeps it open (90 s)",
              "desktop: locked" not in vm.serial()[mark:])

        # Sound playing keeps it open while it plays; the minute counts from
        # when the sound stopped. The terminal has the keyboard again.
        mark = len(vm.serial())
        vm.type("music /home/LONG.WAV\n")
        playing = said_since(mark, "music: playing LONG.WAV", 30)
        c.add("a song is playing", playing is not None)
        done = said_since(mark, "music: finished with", SONG_S * 3)
        c.add("it stays open while the song plays (%d s)" % SONG_S,
              done is not None and "desktop: locked" not in vm.serial()[mark:])
        mark = len(vm.serial())
        at = said_since(mark, "desktop: locked", 240)
        took = (at - done) if (at and done) else -1
        c.add("and locks a minute after it ends (%.0f s)" % took, at is not None and took >= 57)
    finally:
        vm.stop()

    try:
        os.remove(song)
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
