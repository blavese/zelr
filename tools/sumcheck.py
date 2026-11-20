"""The terminal's sha256sum and cmp.

Files of known bytes on a fresh volume; the commands are typed into the
desktop's terminal, each sent into a file with >, and the files read back
off the disk and compared with what Python's hashlib and a byte walk here
say.

  - sha256sum of an empty file, "abc", 55, 56 and 64 bytes (where the
    padding takes one block or two) and of 3 MiB and 17 bytes, read in
    pieces, all on one line;
  - sha256sum after a |, named "-";
  - cmp of a file with itself, with a copy changed past its first 128 KiB
    (byte and line, counted from 1), and with its own first 100000 bytes
    either way round.

  python tools/sumcheck.py [--keep]
"""
import hashlib
import os
import random
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "sumcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "sumcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)

rng = random.Random(222)
BIG = bytes(rng.randrange(256) for _ in range(3 * 1024 * 1024 + 17))
CHANGED_AT = 2 * 65536 + 4321                 # past two of the terminal's pieces
BIG2 = BIG[:CHANGED_AT] + bytes([BIG[CHANGED_AT] ^ 0x5A]) + BIG[CHANGED_AT + 1:]
SHORT = BIG[:100000]

FILES = {
    "e.bin": b"",
    "abc.txt": b"abc",
    "b55.bin": bytes(range(55)),
    "b56.bin": bytes(range(56)),
    "b64.bin": bytes(range(64)),
    "big.bin": BIG,
    "big2.bin": BIG2,
    "short.bin": SHORT,
}
SUMMED = ["e.bin", "abc.txt", "b55.bin", "b56.bin", "b64.bin", "big.bin"]
D = "/home/s/"
LINES = [
    "sha256sum " + " ".join(D + n for n in SUMMED) + " > " + D + "sums.txt",
    "echo hello | sha256sum > " + D + "pipe.txt",
    "cmp %sbig.bin %sbig.bin > %sc1.txt" % (D, D, D),
    "cmp %sbig.bin %sbig2.bin > %sc2.txt" % (D, D, D),
    "cmp %sbig.bin %sshort.bin > %sc3.txt" % (D, D, D),
    "cmp %sshort.bin %sbig.bin > %sc4.txt" % (D, D, D),
    "notice all done",
]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("sha256sum and cmp")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for name, data in FILES.items():
        p = os.path.join(WORK, name)
        with open(p, "wb") as f:
            f.write(data)
        args.append(p + ":home/s/" + name)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with files of known bytes", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "sum-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)
        mark = len(vm.serial())
        for line in LINES:
            vm.type(line + "\n")
        end = time.time() + 180
        done = False
        while time.time() < end and not done:
            done = "notice: all done" in vm.serial()[mark:]
            time.sleep(0.2)
        c.add("the lines all ran", done)
        time.sleep(1)
    finally:
        vm.stop()

    def read(path):
        try:
            got = Fat16(IMAGE).read_file(path)
            return None if got is None else got.decode(errors="replace")
        except Exception:
            return None

    sums = read(D + "sums.txt") or ""
    want = "".join("%s  %s%s\n" % (hashlib.sha256(FILES[n]).hexdigest(), D, n) for n in SUMMED)
    got_lines, want_lines = sums.splitlines(), want.splitlines()
    for g, w in zip(got_lines, want_lines):
        if g != w:
            print("      | got  %a\n      | want %a" % (g, w))
    small_ok = got_lines[:5] == want_lines[:5]
    c.add("sha256sum of 0, 3, 55, 56 and 64 bytes is hashlib's, each with its name", small_ok)
    c.add("sha256sum of 3 MiB and 17 bytes, read in pieces, is hashlib's",
          len(got_lines) == 6 and got_lines[5:] == want_lines[5:])

    pipe = read(D + "pipe.txt")
    print("      | pipe: %a" % pipe)
    c.add("echo hello | sha256sum is the sum of hello and a newline, named -",
          pipe == "%s  -\n" % hashlib.sha256(b"hello\n").hexdigest())

    line = BIG[:CHANGED_AT].count(b"\n") + 1
    outs = [read(D + "c%d.txt" % i) for i in range(1, 5)]
    print("      | cmp: %a" % outs)
    c.add("cmp of a file with itself: the same, and how long",
          outs[0] == "%sbig.bin %sbig.bin are the same, %d bytes\n" % (D, D, len(BIG)))
    c.add("cmp with a byte changed past 128 KiB: that byte and its line, from 1",
          outs[1] == "%sbig.bin %sbig2.bin differ: byte %d, line %d\n" % (D, D, CHANGED_AT + 1, line))
    c.add("cmp with the file's first 100000 bytes, either way round: that one ends first",
          outs[2] == "%sshort.bin ends first, after byte 100000\n" % D
          and outs[3] == "%sshort.bin ends first, after byte 100000\n" % D)

    for name in FILES:
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
