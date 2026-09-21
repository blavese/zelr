"""The six things a program can now do that it could not before.

Floating point, a heap, JavaScript, making another process, and handing
it a descriptor. All five are
ring 3 rather than kernel, so the kernel's own self test cannot reach any of
them: it runs before there is a program, in a kernel compiled without the
vector instructions, with no allocator and no interpreter.

So they are programs, and this runs them. One machine for all five, because
booting is the slow part and none of them cares what the others did.

Each prints its own checks and ends with a marker. The marker is what is
looked for, rather than counting PASS lines, so a suite that dies halfway
through fails here instead of passing with fewer checks than it had
yesterday.

  python tools/ring3check.py [--keep]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT              # noqa: E402

DISK = os.path.join(ROOT, "ring3.%d.img" % os.getpid())

# The program, the marker it prints when every one of its checks passed, and
# how long to give it. JavaScript is slowest because it parses and runs
# eighty-odd scripts, each in a context of its own.
SUITES = [
    ("fptest",    "FPTEST_PASS",    "floating point", 60),
    ("alloctest", "ALLOCTEST_PASS", "the allocator", 90),
    ("jstest",    "JSTEST_PASS",    "javascript", 180),
    ("forktest",  "FORKTEST_PASS",  "fork and exec", 120),
    ("fdtest",    "FDTEST_PASS",    "descriptors", 120),
    ("pagetest",  "PAGETEST_PASS",  "a page and a script on it", 120),
    ("pngtest",   "PNGTEST_PASS",   "pictures", 120),
    ("jpegtest",  "JPEGTEST_PASS",  "photographs", 180),
    ("svgtest",   "SVGTEST_PASS",   "drawings", 180),
    ("layouttest", "LAYOUTTEST_PASS", "where things end up", 120),
    ("cardtest",  "CARDTEST_PASS",  "what a poker hand is worth", 120),
]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("ring 3")

    vm = Guest(DISK, memory=256)
    try:
        vm.wait_boot()

        for name, marker, label, timeout in SUITES:
            out = vm.run("exec /bin/%s" % name, timeout=timeout)

            ran = marker.replace("_PASS", "_FAIL") in out or marker in out
            c.add("%s runs at all" % label, ran)
            c.add("and every one of its checks passed", marker in out)

            if marker not in out:
                # The failing lines, because "it failed" on its own sends
                # the next person to read this into the wrong file.
                shown = 0
                for line in out.splitlines():
                    if "FAIL" in line or "wanted" in line:
                        print("      | %s" % line.strip())
                        shown += 1
                        if shown > 12:
                            print("      | ...")
                            break
                if not shown:
                    for line in out.splitlines()[-6:]:
                        print("      | %s" % line.strip())

        # And the number the JavaScript suite reports, so a suite that
        # quietly stopped running half its cases is visible even when the
        # ones it did run all passed.
        out = vm.run("exec /bin/jstest", timeout=180)
        total = 0
        for line in out.splitlines():
            if " of " in line and "passed" in line:
                try:
                    total = int(line.strip().split(" of ")[1].split()[0])
                except (ValueError, IndexError):
                    total = 0
        print("      javascript: %d cases" % total)
        c.add("and there are as many javascript cases as there were",
              total >= 86)
    finally:
        vm.stop()
        if not keep and os.path.exists(DISK):
            os.remove(DISK)

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
