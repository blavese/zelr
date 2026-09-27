"""A program written in standard C, built outside this repository, run here.

tools/sdkcheck.py already proves that a program built from sdk/ alone runs on
this machine. What it does not prove is the thing a person actually asks: can
you take C that was written for some other computer and run it on this one.
The program it builds is written against sdk/zelr.h, which is zelr's own
interface with zelr's own spellings, and no program in the world was written
against it except the ones in this tree.

So this builds a program whose every line is standard C -- stdio, stdlib,
string, ctype, math -- with no mention of zelr anywhere in it, against
sdk/libc. It copies sdk/ out of the tree first, so a header that quietly
needs a sibling fails here rather than somewhere else later.

Then it writes the result onto a FAT volume this repository made, boots a
machine with that volume as its disk, and types the program's name.

What it checks is not that it ran, but that it was *right*: printf's padding,
qsort's ordering, strtol's parsing, the arithmetic, and a file written and
read back. A libc that runs and prints the wrong number is worse than none.

  python tools/libccheck.py [--keep]
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

SDK = os.path.join(ROOT, "sdk")
ON_DISK = "home/foreign"


def build_outside(work):
    """sdk/ copied whole into somewhere that is not the tree, and built in."""
    shutil.copytree(SDK, os.path.join(work, "sdk"))
    libc = os.path.join(work, "sdk", "libc")

    r = subprocess.run(["bash", "build.sh", "hello.c", "foreign"],
                       cwd=libc, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        return None
    out = os.path.join(libc, "foreign")
    return out if os.path.exists(out) else None


def make_volume(program, image):
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mkfat.py"),
                        image, "32768", "%s:%s" % (program, ON_DISK)],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-1000:])
        print(r.stderr[-1000:])
        return False
    return True


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("a libc, and a program that has never heard of zelr")

    work = tempfile.mkdtemp(prefix="zelr-libc-")
    image = os.path.join(ROOT, "libccheck.%d.img" % os.getpid())
    vm = None
    try:
        program = build_outside(work)
        c.add("standard C builds against sdk/libc, outside the tree",
              program is not None)
        if not program:
            return c.report(keep=keep)

        size = os.path.getsize(program)
        c.add("and what comes out is an executable of a plausible size",
              8192 < size < 600 * 1024)
        print("      %d bytes, built in %s" % (size, work))

        c.add("it can be written onto a FAT volume", make_volume(program, image))

        vm = Guest(image, memory=256, reuse=True)
        vm.wait_boot()

        # Driven rather than run to the prompt, because the program stops
        # half way to read a line and then counts a second by clock(); the
        # second is timed here, by this machine's clock rather than the
        # guest's.
        mark = len(vm.serial())
        want = vm.prompts() + 1
        vm.type("foreign one two\n")
        asked = vm.wait_serial("type a line:", timeout=60)
        if asked:
            vm.type("a line for stdin\n")
        started = vm.wait_serial("clock start", timeout=30)
        t0 = time.time()
        finished = vm.wait_serial("clock done", timeout=40)
        took = time.time() - t0
        vm.wait_prompt(want, timeout=30)
        out = vm.serial()[mark:]

        c.add("typing its name runs it", "LIBC_HELLO_OK" in out)
        if "LIBC_HELLO_OK" not in out:
            print("      got: %r" % out.strip()[-800:])
            return c.report(keep=keep)

        # --- what it printed, which is the whole question -----------------
        c.add("printf writes what it was given",
              "hello from a program with a libc" in out)

        c.add("and the words it was started on arrived as words",
              "argv[0] = foreign" in out and "argv[1] = one" in out
              and "argv[2] = two" in out)

        c.add("strdup, toupper and strlen agree",
              "THE QUICK BROWN FOX (19 characters)" in out)

        c.add("qsort puts them in order",
              "sorted: 1 3 7 19 42 56 88" in out)

        # Width, left alignment, zero padding and a forced sign, which is
        # where a formatter written in an afternoon gets it wrong.
        c.add("printf pads and aligns the way it says it does",
              "7     7 7    | 00007 +7" in out)

        c.add("and knows the other bases",
              "beef BEEF 10 z text 0x1234" in out)

        # 2.5 to no places is 3 because the rounding happens at the place
        # being printed, and -1.5 in a field of eight is right aligned.
        c.add("and prints a double with the places asked for",
              "3.142 3    -1.50" in out)

        c.add("strtol reads a number and stops where the number stops",
              "strtol ok" in out)

        c.add("sqrt is right to six places", "sqrt(2) = 1.414214" in out)
        c.add("pow of a whole power is exact", "pow(2,10) = 1024" in out)
        c.add("sin is right to six places", "sin(pi/6) = 0.500000" in out)

        c.add("a file written with fprintf reads back with fgets",
              "read back: written by a foreign program" in out
              and "read back: second line" in out)

        # atan used another series and came out 0.6046 for atan(1).
        c.add("atan is right to six places", "atan(1)*4 = 3.141593" in out)
        c.add("and atan2 knows its quadrant", "atan2(1,-1) = 2.356194" in out)

        # fputc stored into a buffer fwrite had left full, one byte past its
        # end, which is the descriptor of the FILE next to it.
        c.add("a character after a full buffer does not spill into the next file",
              "next: the next file is intact" in out)
        c.add("and arrives in its own", "fill: 4097 bytes" in out)

        # fseek(SEEK_CUR) went from the descriptor, which the read-ahead had
        # carried to the end of the file, so this read past the end.
        c.add("a seek from here goes from where the program is",
              "seek from here: d at 4" in out)
        # ftell ignored what was buffered and not yet flushed.
        c.add("and where it is counts what is waiting to be written",
              "tell while writing: 3" in out)

        # stdin was a console that read nothing, so this got end of file.
        c.add("stdin reads what was typed", asked and "stdin said: a line for stdin" in out)

        # clock() counted ticks as milliseconds, ten times too slow, so a
        # second by it took ten.
        c.add("a second by clock() is about a second",
              started and finished and 0.4 < took < 5.0)
        print("      a second by clock() took %.1f s here" % took)
    finally:
        if vm:
            vm.stop()
        shutil.rmtree(work, ignore_errors=True)
        if not keep:
            try:
                os.remove(image)
            except OSError:
                pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
