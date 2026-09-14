"""Checks the bootloader's own invariants, which the assembler cannot.

Stage one reads the whole loader off the disc and then checks a signature
before trusting what came back. That check is only worth anything if the
signature sits past the 512 bytes firmware is obliged to load, and inside the
2 KiB stage one asks for. Both are differences between two symbols, which are
not known until the thing is linked, so the assembler cannot test them.

  python tools/check_loader.py build/cdboot.bin
"""
import sys

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


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build/cdboot.bin"))
