"""Checks the bootloader's own invariants, which the assembler cannot.

Stage one reads the whole loader off the disc and then checks a signature
before trusting what came back. That check is only worth anything if the
signature sits past the 512 bytes firmware is obliged to load, and inside the
2 KiB stage one asks for. Both are differences between two symbols, which are
not known until the thing is linked, so the assembler cannot test them.

  python tools/check_loader.py build/cdboot.bin
"""
import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from loadaddr import load_address                        # noqa: E402

SIGNATURE = b"ZLR1"          # 0x31524C5A little-endian
FIRST_SECTOR = 512
STAGE1_READS = 2048


def main(path):
    data = open(path, "rb").read()

    at = data.find(SIGNATURE, FIRST_SECTOR)
    if at < 0:
        # Finding it early is its own failure: it would then be present even
        # after a short read, and prove nothing.
        early = data.find(SIGNATURE)
        if 0 <= early < FIRST_SECTOR:
            print("loader: the signature is at %d, inside the first sector, "
                  "so a short read would still pass it" % early, file=sys.stderr)
        else:
            print("loader: no signature in %s" % path, file=sys.stderr)
        return 1

    if at + len(SIGNATURE) > STAGE1_READS:
        print("loader: the signature is at %d, past the %d bytes stage one "
              "reads" % (at, STAGE1_READS), file=sys.stderr)
        return 1

    print("      signature at %d, checked after a short read and inside the "
          "%d stage one asks for" % (at, STAGE1_READS))
    return 0


def check_handoff(root):
    """The handoff magic is one constant kept in two places.

    include/handoff.h defines it as a 64-bit number. cdboot.S is 16-bit code
    with no 64-bit stores, so it writes the low half and the high half
    separately, as two literals with no connection to the header at all.
    Edit one and the kernel rejects a handoff that is correct in every other
    respect, and the only symptom is a machine that stops after the loader.

    Nothing checked this. It went wrong during the rename to zelr, and it
    was found by searching the tree for the old value in hex rather than by
    anything here.
    """
    hdr = io.open(os.path.join(root, "include", "handoff.h"), encoding="utf-8").read()
    m = re.search(r"#define\s+HANDOFF_MAGIC\s+0x([0-9A-Fa-f]+)u?ll", hdr)
    if not m:
        print("loader: no HANDOFF_MAGIC in include/handoff.h", file=sys.stderr)
        return 1
    want = int(m.group(1), 16)

    boot = io.open(os.path.join(root, "bootloader", "cdboot.S"), encoding="utf-8").read()
    # Anchored to the end of the line so the low half is not matched by
    # the high half's line, which begins with the same text.
    lo = re.search(r"movl\s+\$0x([0-9A-Fa-f]{1,8}), %es:H_MAGIC\s*$", boot, re.M)
    hi = re.search(r"movl\s+\$0x([0-9A-Fa-f]{1,8}), %es:H_MAGIC \+ 4\s*$", boot, re.M)
    if not lo or not hi:
        print("loader: cdboot.S does not write H_MAGIC in two halves",
              file=sys.stderr)
        return 1

    got = int(hi.group(1), 16) << 32 | int(lo.group(1), 16)
    if got != want:
        print("loader: cdboot.S writes %016X, the kernel expects %016X"
              % (got, want), file=sys.stderr)
        return 1

    print("      handoff magic %016X, written in halves that agree" % want)
    return 0


def check_load_address(root):
    """Where the kernel is linked, written out in three languages.

    linker.ld decides it. build.sh and tools/mkiso.py read it from there, so
    they cannot drift. The other two cannot ask: the UEFI loader needs a C
    constant, and the real mode loader needs a word in its own header that
    the firmware can be told to load. mkiso.py patches that word, so the
    literal in the assembly is only what an unpatched loader would use -- but
    an unpatched loader is exactly what a partial build produces.

    What disagreement looks like is worth stating, because it is not an error
    message. A loader that copies the kernel to the wrong address does not
    discover this. It jumps to the entry point it was given, and executes
    whatever is there.
    """
    want = load_address(root)

    ldr = io.open(os.path.join(root, "uefi", "loader.c"), encoding="utf-8").read()
    m = re.search(r"#define\s+KERNEL_PHYS\s+0x([0-9A-Fa-f]+)u?ll", ldr)
    if not m:
        print("loader: no KERNEL_PHYS in uefi/loader.c", file=sys.stderr)
        return 1
    if int(m.group(1), 16) != want:
        print("loader: uefi/loader.c loads at 0x%X, linker.ld links at 0x%X"
              % (int(m.group(1), 16), want), file=sys.stderr)
        return 1

    boot = io.open(os.path.join(root, "bootloader", "cdboot.S"), encoding="utf-8").read()
    m = re.search(r"^patch_load:\s+\.long\s+0x([0-9A-Fa-f]+)", boot, re.M)
    if not m:
        print("loader: cdboot.S has no patch_load", file=sys.stderr)
        return 1
    if int(m.group(1), 16) != want:
        print("loader: cdboot.S defaults to 0x%X, linker.ld links at 0x%X"
              % (int(m.group(1), 16), want), file=sys.stderr)
        return 1

    print("      linked at 0x%X, and both loaders agree" % want)
    return 0


if __name__ == "__main__":
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    rc = main(sys.argv[1] if len(sys.argv) > 1 else "build/cdboot.bin")
    sys.exit(rc or check_handoff(here) or check_load_address(here))
