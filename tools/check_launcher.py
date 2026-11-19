"""Checks that the ways of starting zelr on Windows give it room to browse.

The launcher (launcher/Emulator.cs) started the machine with 64 MiB, a figure
from when the browser was a fraction of its size. Measured under QEMU on
2026-11-19 with an RTL8139, as the launcher has it: at 64 MiB the browser ran
out of memory on the first page it loaded (example.com) and its window
vanished; at 128 it ran out on Wikipedia, at 256 on GitHub; at 512 it loaded
all three. The gate never saw it: every harness that runs the browser gives
the machine 256 MiB.

So every -m in the launcher and in zelr.bat must be 512 or more. run.sh is
left alone: its 64 MiB is the small machine the self test is counted on.

  python tools/check_launcher.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEAST = 512


def main():
    bad, seen = [], 0
    cs = open(os.path.join(ROOT, "launcher", "Emulator.cs"), encoding="utf-8").read()
    for m in re.finditer(r'ArgumentList\.Add\("-m"\);\s*psi\.ArgumentList\.Add\("(\d+)"\)', cs):
        seen += 1
        if int(m.group(1)) < LEAST:
            bad.append("launcher/Emulator.cs: -m %s" % m.group(1))
    bat = open(os.path.join(ROOT, "zelr.bat"), encoding="utf-8").read()
    for line in bat.splitlines():
        if line.lstrip().lower().startswith("rem"):
            continue
        for m in re.finditer(r"(?:^|\s)-m (\d+)", line):
            seen += 1
            if int(m.group(1)) < LEAST:
                bad.append("zelr.bat: -m %s" % m.group(1))
    if not seen:
        print("no -m found in the launcher or zelr.bat: the check reads nothing")
        return 1
    if bad:
        for b in bad:
            print("too little memory to browse (%d MiB at least): %s" % (LEAST, b))
        return 1
    print("the launcher and zelr.bat give the machine %d MiB or more (%d places)" % (LEAST, seen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
