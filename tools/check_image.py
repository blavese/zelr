"""Whether the kernel image leaves a small machine room for its programs.

Every program in /bin is pasted into the kernel image (kernel/builtin.S), and
the image is loaded whole at 16 MiB. What is left above it on a machine with
64 MiB is the kernel's heap and every page a program is given. The image
grew from 14 MB to 27 MB in two releases, most of it debug information the
compiler added to each program by default and nothing ever reads, and on the
64 MiB machine run.sh starts, the heap came out 20 MiB instead of 24, the
desktop's copy of its wallpaper no longer fitted, and a program asking for
memory found almost none.

So this reads each program built for the image and fails if any carries a
.debug section (userland/build.sh strips them), and fails if the flat image
is past IMAGE_BUDGET, which is what leaves a 64 MiB machine its whole heap and
room besides.

  python tools/check_image.py
"""
import glob
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 16 MiB of image on a 64 MiB machine: loaded at 16, it ends at 32, and the
# heap's 24 MiB and a screen's worth leave some 8 MiB of pages for programs.
IMAGE_BUDGET = 16 * 1024 * 1024


def section_names(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF" or d[4] != 2:
        return None
    shoff = struct.unpack_from("<Q", d, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    if not shoff or shstrndx >= shnum:
        return []
    hdrs = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    so = hdrs[shstrndx][4]
    names = []
    for h in hdrs:
        end = d.index(b"\0", so + h[0])
        names.append((d[so + h[0]:end].decode("ascii", "replace"), h[5]))
    return names


def main():
    failed = 0
    progs = sorted(glob.glob(os.path.join(ROOT, "build", "user", "*.elf")))
    if not progs:
        print("  FAIL  no programs built (build/user is empty)")
        return 1
    heavy = []
    for p in progs:
        names = section_names(p)
        if names is None:
            print("  FAIL  %s is not a 64 bit ELF" % os.path.basename(p))
            failed += 1
            continue
        debug = sum(size for name, size in names if name.startswith(".debug"))
        if debug:
            heavy.append((debug, os.path.basename(p)))
    if heavy:
        heavy.sort(reverse=True)
        print("  FAIL  %d programs carry debug information, %d bytes of it (%s ...)"
              % (len(heavy), sum(h[0] for h in heavy), ", ".join(h[1] for h in heavy[:4])))
        failed += 1
    else:
        print("  PASS  no program in the image carries debug information (%d programs)" % len(progs))

    image = os.path.join(ROOT, "build", "zelr.bin")
    size = os.path.getsize(image) if os.path.exists(image) else -1
    if size < 0:
        print("  FAIL  build/zelr.bin is not there")
        failed += 1
    elif size > IMAGE_BUDGET:
        print("  FAIL  the image is %d bytes, past its budget of %d" % (size, IMAGE_BUDGET))
        failed += 1
    else:
        print("  PASS  the image is %d bytes, within its budget of %d" % (size, IMAGE_BUDGET))
    print("the image: %s" % ("all 2 checks passed" if not failed else "%d failed" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
