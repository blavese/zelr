"""The programs that go inside the kernel image, packed.

  python tools/packprogs.py build/user

Each NAME.elf there becomes NAME.elf.z beside it, which is what
kernel/builtin.S pastes into the image: "ZPK1", the length of the program
and its CRC-32 (four bytes each, least significant first), then the program
as raw deflate (RFC 1951). The kernel inflates a program when it is read
(kernel/sysfs.c) and refuses one whose length or CRC does not come out right.

Why: the programs are most of the image, and the image is loaded whole into
the memory a small machine has. Packed they are about two fifths of their
size: 13.7 MB of programs came to 5.7 MB, which is room for the image to
grow and memory a 64 MiB machine gets back for its programs.

The deflate is the host's zlib, a tool like the compiler; what reads it is
the kernel's own inflate (userland/inflate.h). Level 9, which is the same
bytes for the same program every time, so an unchanged program packs to an
unchanged file and the kernel build's checksum of them does not move.
"""
import glob
import os
import struct
import sys
import zlib


def pack(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    body = c.compress(data) + c.flush()
    return b"ZPK1" + struct.pack("<II", len(data), zlib.crc32(data) & 0xFFFFFFFF) + body


def main():
    where = sys.argv[1] if len(sys.argv) > 1 else os.path.join("build", "user")
    total = packed = 0
    for path in sorted(glob.glob(os.path.join(where, "*.elf"))):
        with open(path, "rb") as f:
            data = f.read()
        out = pack(data)
        # Written only when it differs, so the file's time says when the
        # program last changed.
        dest = path + ".z"
        try:
            with open(dest, "rb") as f:
                same = f.read() == out
        except OSError:
            same = False
        if not same:
            with open(dest, "wb") as f:
                f.write(out)
        total += len(data)
        packed += len(out)
    if not total:
        print("packprogs: no programs in %s" % where)
        return 1
    print("packprogs: %d bytes of programs packed to %d" % (total, packed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
