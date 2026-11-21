"""Programs in a folder deeper than 127 bytes.

Paths have been 255 bytes since 0.210.0, but /bin/ls listed / from a
folder deeper than 127 and Music cut a song's path there. A WAV twenty
folders down (a path of 194 bytes) on a fresh volume; the terminal goes
there in two cds and names it in two sets, since a line typed holds 160.

  - pwd says the whole path;
  - /bin/ls with nothing after it lists that folder, not /;
  - Music started on the song's whole path plays it to the end.

  python tools/deepcheck.py [--keep]
"""
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "deepcheck.%d.img" % os.getpid())
WAV = os.path.join(ROOT, "deepcheck.%d.wav" % os.getpid())
REC = os.path.join(ROOT, "deepcheck.%d.rec.wav" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
AUDIO = ["-audiodev", "wav,id=a0,path=" + REC.replace("\\", "/"),
         "-device", "intel-hda", "-device", "hda-output,audiodev=a0"]

PART = "abcdefgh"
HALF = "/".join([PART] * 10)
DEEP = "/home/" + HALF + "/" + HALF               # 6 + 89 + 1 + 89 = 185 bytes
SONG = DEEP + "/song.wav"


def wav():
    rate, n = 48000, 48000 // 3
    body = b"".join(struct.pack("<hh", v, v) for v in ((6000 if (i // 50) % 2 else -6000) for i in range(n)))
    return (b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16)
            + b"data" + struct.pack("<I", len(body)) + body)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("deep folders")
    with open(WAV, "wb") as f:
        f.write(wav())
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", WAV + ":" + SONG.lstrip("/")],
                       cwd=ROOT, capture_output=True, text=True)
    os.remove(WAV)
    c.add("a song %d bytes down" % len(SONG), r.returncode == 0 and len(SONG) > 128)
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
            "deep-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def said_since(mark, text, timeout=30):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        mark = len(vm.serial())
        for line in ("cd /home/" + HALF, "cd " + HALF, "pwd > /home/pwd.txt", "notice went down"):
            vm.type(line + "\n")
        said_since(mark, "notice: went down")

        mark = len(vm.serial())
        vm.type("/bin/ls\n")
        listed = said_since(mark, "song.wav", 20)
        out = vm.serial()[mark:]
        print("      | /bin/ls said %a" % out[-300:])
        c.add("/bin/ls with nothing after it lists the deep folder, not /", listed)

        mark = len(vm.serial())
        for line in ("set a /home/" + HALF, "set b $a/" + HALF, "music $b/song.wav"):
            vm.type(line + "\n")
        c.add("Music started on the song's whole path plays it to the end",
              said_since(mark, "music: finished with", 60))
        mon.send("sendkey alt-q", settle=1.0)
    finally:
        vm.stop()

    try:
        got = (Fat16(IMAGE).read_file("/home/pwd.txt") or b"").decode(errors="replace")
    except Exception:
        got = None
    print("      | pwd: %a" % got)
    c.add("pwd says the whole path", got == DEEP + "\n")

    for junk in [REC] + ([] if keep else [IMAGE]):
        try:
            os.remove(junk)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
