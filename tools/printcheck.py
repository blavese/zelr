"""Print Screen, and the picture it saves held up against the screen.

On the desktop the key saves what the screen shows as a PNG in
/home/pictures, named by the time (kernel/shot.c, with a deflate of its
own). This presses it through the monitor, reads the file back off the disk
image on this side, takes it apart with the standard library (each chunk's
CRC, zlib, the row filters) and compares it pixel by pixel with QEMU's own
dump of the screen taken straight after. A cursor that blinks between the
two is allowed for: almost every pixel, not every one.

The desktop also says it on the screen, in a notice above the dock for a
few seconds: that is looked for as a change above the dock after the key,
and the comparison waits until the notice has gone again.

  python tools/printcheck.py [--keep]
"""
import datetime
import os
import re
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT          # noqa: E402
from readfat import Fat16                                              # noqa: E402

DISK = os.path.join(ROOT, "printcheck.%d.img" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)          # the terminal's default background
SCREEN = (0, 0, 1024, 768)
ABOVE_DOCK = (262, 644, 762, 710)       # where the notice comes and goes


def crop(px, w, rect):
    left, top, right, bottom = rect
    return b"".join(px[(y * w + left) * 3:(y * w + right) * 3] for y in range(top, bottom))


def png_pixels(data):
    """(width, height, rgb bytes, every CRC good) from a PNG of 8-bit RGB."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    at, idat, w, h, good = 8, b"", 0, 0, True
    while at + 12 <= len(data):
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        crc, = struct.unpack(">I", data[at + 8 + n:at + 12 + n])
        good = good and zlib.crc32(kind + body) & 0xFFFFFFFF == crc
        if kind == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", body[:10])
            if depth != 8 or colour != 2:
                return None
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        at += 12 + n
    try:
        raw = zlib.decompress(idat)
    except zlib.error as e:             # what a broken deflate or Adler-32 comes to
        print("    zlib: %s" % e)
        return None
    stride = 3 * w
    out = bytearray(stride * h)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for k in range(stride):
            a = line[k - 3] if k >= 3 else 0
            b = prev[k]
            c = prev[k - 3] if k >= 3 else 0
            if f == 1:
                line[k] = (line[k] + a) & 255
            elif f == 2:
                line[k] = (line[k] + b) & 255
            elif f == 3:
                line[k] = (line[k] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[k] = (line[k] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bytes(out), good


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("print screen")
    vm = Guest(DISK, memory=256, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)               # out of the way
        _, _, _, shot, up = mon.wait_screen(
            "print-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(2)                      # nothing still opening
        w0, h0, px0, _, _ = mon.wait_screen("print-before", lambda w, h, px: True)
        quiet = crop(px0, w0, ABOVE_DOCK)

        mark = len(vm.serial())
        mon.send("sendkey print", settle=0.2)
        end = time.time() + 60
        path = None
        while time.time() < end and not path:
            m = re.search(r"desktop: the screen is saved in (\S.*\.png)", vm.serial()[mark:])
            path = m.group(1).strip() if m else None
            time.sleep(0.2)
        host = datetime.datetime.utcnow()
        _, _, _, shot, told = mon.wait_screen(
            "print-notice", lambda w, h, px: crop(px, w, ABOVE_DOCK) != quiet, timeout=10)
        c.add("a notice above the dock says so", told, shot)
        _, _, _, shot, gone = mon.wait_screen(
            "print-gone", lambda w, h, px: crop(px, w, ABOVE_DOCK) == quiet, timeout=15)
        c.add("and goes again after a few seconds", gone, shot)
        w, h, px, shot, _ = mon.wait_screen("print-after", lambda w, h, px: True)
        c.add("Print Screen says where it saved the screen (%s)" % path, path is not None)
        if not path:
            print(vm.serial()[mark:][-600:])
            return c.report(keep=keep)

        named = re.search(r"screen (\d{4})-(\d\d)-(\d\d) (\d\d)\.(\d\d)\.(\d\d)\.png$", path)
        when = datetime.datetime(*map(int, named.groups())) if named else None
        c.add("named by when it was taken (%s, here %s)" % (when, host.strftime("%H:%M:%S")),
              when is not None and abs((when - host).total_seconds()) <= 5)
    finally:
        vm.stop()

    data = None
    try:
        data = Fat16(DISK).read_file(path)
    except Exception as e:
        print("reading it: %s" % e)
    c.add("the file is on the disk (%s bytes)" % (len(data) if data else None), bool(data))
    got = png_pixels(data) if data else None
    c.add("it is a PNG of the whole screen, 1024 by 768, every chunk's checksum good",
          got is not None and got[0] == 1024 and got[1] == 768 and got[3])
    c.add("and much smaller than the screen it holds (a quarter at most)",
          bool(data) and len(data) < 1024 * 768 * 3 // 4)
    if got and got[0] == w and got[1] == h:
        same = sum(1 for i in range(0, len(px), 3) if px[i:i + 3] == got[2][i:i + 3])
        total = w * h
        c.add("and it is the screen, pixel for pixel (%d of %d the same)" % (same, total),
              same >= total - total // 1000)
    else:
        c.add("and it is the screen, pixel for pixel", False)

    if not keep:
        try:
            os.remove(DISK)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
