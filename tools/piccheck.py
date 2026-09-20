"""Runs the five headless checks that live inside the machine.

The picture decoders, the rasteriser and the layout are all pure arithmetic:
bytes in, bytes out, and nothing about a screen. They are checked by ring 3
programs that compare against answers worked out elsewhere -- the PNGs and
JPEGs come from the host's own libraries through tools/genpng.py and
tools/genjpeg.py, because a decoder checked against its own encoder agrees
with itself and proves nothing.

Those programs existed and nothing ran them. Ninety odd checks, written
while the decoders were being built, sitting in the image where only
somebody who already knew to type their names would find them. So this runs
all five and reads the one line each of them ends with.

One boot for the five, because a boot is the cost and the checks are a
second apiece.

  python tools/piccheck.py [--keep]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT       # noqa: E402

DISK = os.path.join(ROOT, "piccheck.%d.img" % os.getpid())

# The program, what it is about, and the word it ends with. The sentinel is
# printed by the program itself and is the only thing read here: counting
# PASS lines would also count the word PASS inside the name of a check.
# exec rather than the bare name: the console shell runs a program only
# when it is told to, and waits for it, which is what keeps the output from
# arriving after the next prompt.
PROGRAMS = [
    ("pngtest", "the picture decoder reads pngs it did not make", "PNGTEST"),
    ("jpegtest", "and jpegs, through the huffman and the idct", "JPEGTEST"),
    ("svgtest", "the rasteriser fills paths where the paths are", "SVGTEST"),
    ("layouttest", "the layout puts boxes where the page asks", "LAYOUTTEST"),
    ("pagetest", "and a whole page parses, styles and lays out", "PAGETEST"),
]


def main():
    keep = "--keep" in sys.argv
    build_once()
    vm = Guest(DISK, memory=256)
    c = Checks("the parts with no screen")

    try:
        vm.wait_boot()
        for program, what, sentinel in PROGRAMS:
            out = vm.run("exec /bin/" + program, timeout=90)
            passed = (sentinel + "_PASS") in out
            c.add(what, passed)
            if not passed:
                # The failing lines, which is what somebody reading this
                # actually needs. A report that says a decoder is broken
                # and not which of its forty checks failed sends whoever
                # reads it back to run the thing by hand.
                for line in out.split("\n"):
                    if "FAIL" in line:
                        print("      " + line.strip())
    finally:
        vm.stop()

    # report() hands back an exit code -- nought for a clean run -- rather
    # than a boolean. Read as one, a passing run exits 1, and the gate
    # printed "all 10 checks passed" beside the word FAIL.
    code = c.report(keep=keep)
    if not keep:
        try:
            os.remove(DISK)
        except OSError:
            pass
    sys.exit(code)


if __name__ == "__main__":
    main()
