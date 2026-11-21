"""The calculator's sums, square roots and memory, read off its tape.

The calculator says each sum it works out on the console ("calc: 78 / 4 =
19.5"); keys are typed to it over the serial line once its window is on the
screen (its operator keys are the accent), and each line is compared with
the answer worked out here in Python, in millionths as the calculator keeps
them.

  - a division, and 0.1 + 0.2, which is 0.3 exactly;
  - square roots of 2 and of a million, to the millionth below;
  - M+ twice, MR into a sum, and MC, after which MR gives nought;
  - a division by nought and the square root of a number below it, which
    it cannot.

  python tools/calccheck.py [--keep]
"""
import math
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "calccheck.%d.img" % os.getpid())
SEED = os.path.join(ROOT, "calccheck.%d.txt" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
ACCENT = (0x6E, 0x8A, 0xE8)


def millionths(text):
    whole, _, frac = text.partition(".")
    return int(whole) * 10 ** 6 + int((frac + "000000")[:6]) * (1 if not whole.startswith("-") else -1)


def shown(m):
    sign = "-" if m < 0 else ""
    m = abs(m)
    whole, frac = divmod(m, 10 ** 6)
    s = "%d" % whole
    if frac:
        s += "." + ("%06d" % frac).rstrip("0")
    return sign + s


def root(text):
    return shown(math.isqrt(millionths(text) * 10 ** 6))


STEPS = [
    ("78/4\n", "calc: 78 / 4 = %s" % shown(millionths("78") * 10 ** 6 // millionths("4")), "a division"),
    ("c0.1+0.2\n", "calc: 0.1 + 0.2 = %s" % shown(millionths("0.1") + millionths("0.2")), "0.1 + 0.2"),
    ("c2r", "calc: sqrt 2 = %s" % root("2"), "the square root of 2"),
    ("c1000000r", "calc: sqrt 1000000 = %s" % root("1000000"), "the square root of a million"),
    ("c9m", "calc: memory 9", "M+ of 9"),
    ("4m", "calc: memory 13", "M+ of 4 more"),
    ("cM*3\n", "calc: 13 x 3 = 39", "MR into a sum"),
    ("L", "calc: memory 0", "MC"),
    # The line MC prints says nought whatever is kept, so what is kept is
    # asked for again.
    ("cM+5\n", "calc: 0 + 5 = 5", "MR after MC"),
    ("c5/0\n", "calc: 5 / 0 = cannot", "a division by nought"),
    ("c4~r", "calc: sqrt -4 cannot", "the square root of -4"),
]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the calculator")
    with open(SEED, "w") as f:
        f.write("seed")
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", SEED + ":home/seed.txt"],
                       cwd=ROOT, capture_output=True, text=True)
    os.remove(SEED)
    c.add("a fresh volume", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    got = {}
    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "calc-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)
        w, h, px, _ = mon.screen("calc-before")
        before = count_in(px, w, SCREEN, ACCENT)
        vm.type("calc\n")
        _, _, _, shot, up = mon.wait_screen(
            "calc-up", lambda w, h, px: count_in(px, w, SCREEN, ACCENT) > before + 8000, timeout=60)
        c.add("the calculator's window comes up, its operator keys in the accent", up, shot)
        time.sleep(1)
        for keys, want, what in STEPS:
            mark = len(vm.serial())
            vm.type(keys)
            end = time.time() + 15
            while time.time() < end and want + "\r" not in vm.serial()[mark:] and want + "\n" not in vm.serial()[mark:]:
                time.sleep(0.2)
            out = vm.serial()[mark:]
            got[what] = want + "\r" in out or want + "\n" in out
            if not got[what]:
                print("      | %s: wanted %a, the console said %a" % (what, want, [l for l in out.splitlines() if "calc:" in l]))
        mon.send("sendkey alt-q", settle=1.0)
    finally:
        vm.stop()

    def all_of(*whats):
        return all(got.get(w) for w in whats)

    c.add("it divides, and 0.1 + 0.2 is 0.3 exactly", all_of("a division", "0.1 + 0.2"))
    c.add("square roots to the millionth below", all_of("the square root of 2", "the square root of a million"))
    c.add("M+ adds what is shown to what is kept, and says it", all_of("M+ of 9", "M+ of 4 more"))
    c.add("MR shows what is kept, into a sum", all_of("MR into a sum"))
    c.add("MC clears it", all_of("MC", "MR after MC"))
    c.add("a division by nought and a square root below it cannot", all_of("a division by nought", "the square root of -4"))

    if not keep:
        try:
            os.remove(IMAGE)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
