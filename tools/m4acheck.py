"""Music plays AAC: an .m4a and an .aac, heard at the pitch in each channel.

The sound is the AAC test's first: 440 Hz on the left and 660 on the right,
a second of it at 44100 Hz, encoded by Windows' own AAC encoder
(tools/genaac.py), its frames kept in userland/aacdata.h. Here the frames are
put into the two files Music is given: an M4A (ftyp, moov with the sample
tables, mdat, written here box by box), and an ADTS stream (a seven-byte
header on each frame). What comes out of the machine is recorded and each
channel measured, as appcheck measures a WAV: a player that parses the file
and plays silence, or plays it at the wrong rate, or puts one channel in
both, cannot pass.

  - Music says it plays each, as AAC at 44100 Hz in stereo, and finishes;
  - both are heard, the left at 440 Hz and the right at 660;
  - a file named .m4a that is not one is refused, saying so;
  - Files calls both a sound.

  python tools/m4acheck.py [--keep]
"""
import os
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT      # noqa: E402
from soundcheck import read_wav, loud_runs, frequency_of, close_to  # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "m4acheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "m4acheck.%d" % os.getpid())
REC = os.path.join(ROOT, "m4arec.%d.wav" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
LEFT_HZ, RIGHT_HZ = 440, 660

AUDIO = ["-audiodev", "wav,id=a0,path=" + REC.replace("\\", "/"),
         "-device", "intel-hda",
         "-device", "hda-output,audiodev=a0"]


def first_case():
    """The config and frames of aacdata.h's first stream."""
    src = open(os.path.join(ROOT, "userland", "aacdata.h"), encoding="utf-8").read()

    def array(name):
        m = re.search(r"%s\[\d*\] = \{(.*?)\};" % name, src, re.S)
        return [int(x, 0) for x in m.group(1).split(",") if x.strip()]
    config = bytes(array("AACD_CONFIG0"))
    sizes = array("AACD_SIZES0")
    data = bytes(array("AACD_DATA0"))
    frames, at = [], 0
    for s in sizes:
        frames.append(data[at:at + s])
        at += s
    return config, frames


def box(kind, body):
    return struct.pack(">I", 8 + len(body)) + kind + body


def full(kind, flags, body):
    return box(kind, struct.pack(">I", flags) + body)


def desc(tag, body):
    return bytes([tag, len(body)]) + body


MATRIX = struct.pack(">9I", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000)


def make_m4a(config, frames, rate, chans):
    """A plain (not fragmented) M4A: one track, one chunk."""
    n = len(frames)
    media = n * 1024
    ms = media * 1000 // rate
    esds = full(b"esds", 0, desc(3, struct.pack(">HB", 1, 0)
                                + desc(4, bytes([0x40, 0x15, 0, 0, 0]) + struct.pack(">II", 128000, 128000)
                                       + desc(5, config))
                                + desc(6, b"\x02")))
    mp4a = box(b"mp4a", bytes(6) + struct.pack(">H", 1) + bytes(8)
               + struct.pack(">HHHHI", chans, 16, 0, 0, rate << 16) + esds)
    stsd = full(b"stsd", 0, struct.pack(">I", 1) + mp4a)
    stts = full(b"stts", 0, struct.pack(">III", 1, n, 1024))
    stsc = full(b"stsc", 0, struct.pack(">IIII", 1, 1, n, 1))
    stsz = full(b"stsz", 0, struct.pack(">II", 0, n) + b"".join(struct.pack(">I", len(f)) for f in frames))

    def moov(offset):
        stco = full(b"stco", 0, struct.pack(">II", 1, offset))
        stbl = box(b"stbl", stsd + stts + stsc + stsz + stco)
        dinf = box(b"dinf", full(b"dref", 0, struct.pack(">I", 1) + full(b"url ", 1, b"")))
        minf = box(b"minf", full(b"smhd", 0, bytes(4)) + dinf + stbl)
        hdlr = full(b"hdlr", 0, bytes(4) + b"soun" + bytes(12) + b"sound\0")
        mdhd = full(b"mdhd", 0, struct.pack(">IIIIHH", 0, 0, rate, media, 0x55C4, 0))
        tkhd = full(b"tkhd", 7, struct.pack(">IIIII", 0, 0, 1, 0, ms) + bytes(8)
                    + struct.pack(">HHHH", 0, 1, 0x100, 0) + MATRIX + bytes(8))
        mvhd = full(b"mvhd", 0, struct.pack(">IIIIIH", 0, 0, 1000, ms, 0x10000, 0x100) + bytes(10)
                    + MATRIX + bytes(24) + struct.pack(">I", 2))
        return box(b"moov", mvhd + box(b"trak", tkhd + box(b"mdia", mdhd + hdlr + minf)))

    ftyp = box(b"ftyp", b"M4A " + struct.pack(">I", 0) + b"M4A mp42isom")
    offset = len(ftyp) + len(moov(0)) + 8
    return ftyp + moov(offset) + box(b"mdat", b"".join(frames))


def make_adts(config, frames):
    """The same frames as an ADTS stream, as a .aac file is."""
    aot = config[0] >> 3
    sfi = ((config[0] & 7) << 1) | (config[1] >> 7)
    chans = (config[1] >> 3) & 15
    out = bytearray()
    for f in frames:
        n = len(f) + 7
        out += bytes([0xFF, 0xF1, ((aot - 1) << 6) | (sfi << 2) | (chans >> 2),
                      ((chans & 3) << 6) | (n >> 11), (n >> 3) & 0xFF, ((n & 7) << 5) | 0x1F, 0xFC])
        out += f
    return bytes(out)


def sounds(samples, rate):
    """The stretches of sound, a run broken by less than half a second taken
    as one: a busy host can leave a gap in a recording, not in the file."""
    groups = []
    for start, end in loud_runs(samples, rate):
        if groups and start - groups[-1][1] < rate // 2:
            groups[-1][1] = end
        else:
            groups.append([start, end])
    return groups


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("music plays aac")
    os.makedirs(WORK, exist_ok=True)
    config, frames = first_case()
    files = {
        "TONE.M4A": make_m4a(config, frames, 44100, 2),
        "TONE.AAC": make_adts(config, frames),
        "JUNK.M4A": b"this is words, not an m4a\n" * 20,
    }
    args = []
    for name, data in files.items():
        src = os.path.join(WORK, name)
        with open(src, "wb") as f:
            f.write(data)
        args.append(src + ":home/" + name)
    for stale in (REC,):
        if os.path.exists(stale):
            os.remove(stale)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with an .m4a and an .aac of the same sound", r.returncode == 0)
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
            "m4a-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def said_since(mark, text, timeout=30):
            end = time.time() + timeout
            while time.time() < end:
                if text.lower() in vm.serial()[mark:].lower():
                    return True
                time.sleep(0.2)
            return False

        # Each played to its end, then its window closed, so that the two are
        # apart in the recording.
        for name in ("TONE.M4A", "TONE.AAC"):
            mark = len(vm.serial())
            vm.type("music /home/%s\n" % name)
            c.add("Music plays %s as AAC, 44100 Hz, stereo" % name,
                  said_since(mark, "music: playing %s, AAC, 44100 Hz, stereo" % name))
            c.add("and finishes it", said_since(mark, "music: finished with", 60))
            time.sleep(2)
            mon.send("sendkey alt-q", settle=1.0)

        mark = len(vm.serial())
        vm.type("music /home/JUNK.M4A\n")
        c.add("a file called .m4a that is not one is refused, saying so",
              said_since(mark, "music: cannot play JUNK.M4A: not an m4a this can read"))
        mon.send("sendkey alt-q", settle=1.0)

        mark = len(vm.serial())
        vm.type("files /home\n")
        said_since(mark, "files: /home")
        for _ in range(6):
            mon.send("sendkey down", settle=0.4)
        out = vm.serial()[mark:].lower()
        c.add("Files calls an .m4a and an .aac a sound",
              "files: tone.m4a - sound" in out and "files: tone.aac - sound" in out)
        mon.send("sendkey alt-q", settle=1.0)
    finally:
        vm.stop()

    rate, left = read_wav(REC, 0) if os.path.exists(REC) else (0, [])
    _, right = read_wav(REC, 1) if os.path.exists(REC) else (0, [])
    heard = sounds(left, rate) if left else []
    c.add("two sounds were recorded (%d)" % len(heard), len(heard) == 2)
    for (start, end), name in zip(heard, ("the .m4a", "the .aac")):
        lhz = frequency_of(left[start:end], rate)
        rhz = frequency_of(right[start:end], rate)
        c.add("%s at %d Hz on the left and %d on the right (heard %d and %d)"
              % (name, LEFT_HZ, RIGHT_HZ, int(lhz), int(rhz)),
              close_to(lhz, LEFT_HZ) and close_to(rhz, RIGHT_HZ))

    for name in files:
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
