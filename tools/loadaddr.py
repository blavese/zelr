"""Where the kernel is linked, read from the one place that decides it.

The address appeared in five places: the linker script, the flat image both
bootloaders copy into memory, the ISO builder that patches the real mode
loader with it, the UEFI loader, and the default sitting in the assembly.

Five copies of an address is five chances to flatten a kernel for one address
and jump to it at another, and that is not a failure anybody gets to read. It
is a machine that starts executing whatever happens to be at the entry point,
which on a fresh boot is zeroes, and zeroes decode to something.

So the linker script decides, everything that can ask asks, and
tools/check_loader.py checks the two that cannot.

  python tools/loadaddr.py        ->  0x1000000
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SCALE = {"": 1, "K": 1024, "M": 1024 ** 2, "G": 1024 ** 3}


def load_address(root=ROOT):
    """The first location assignment in the linker script, which is where the
    kernel is linked. ld spells it `. = 16M;`."""
    text = io_read(os.path.join(root, "linker.ld"))
    m = re.search(r"^\s*\.\s*=\s*(\d+)([KMG]?)\s*;", text, re.M)
    if not m:
        raise SystemExit("loadaddr: linker.ld does not say where the kernel "
                         "is linked")
    return int(m.group(1)) * SCALE[m.group(2)]


def io_read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


if __name__ == "__main__":
    sys.stdout.write("0x%X\n" % load_address())
