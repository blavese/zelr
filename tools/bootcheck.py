"""The disk this system formats, offered to a BIOS as something to start from.

A boot sector ends with 0x55 0xAA, and that is a promise. It tells the
firmware the sector can be loaded to 0x7C00 and jumped to. Any formatter
that writes that mark therefore has to write a program behind it, even on a
disk that holds nothing but files -- because the firmware does not know that
and will jump anyway.

This one wrote the mark and left the rest of the sector empty, so the jump
landed on four hundred bytes of nought, which decodes as an instruction and
runs, and then ran off the end of the sector. The first boot of a machine
formats its disk; every boot after it found a disk the firmware believed in
and could not run. What that looks like is not a filesystem problem, it is a
machine that will not start again, with a message about invalid memory.

So this formats a disk the way a real first boot does, and then asks a plain
BIOS to start from it -- which is the thing that was broken, and which no
check that boots the kernel directly ever reaches.

  python tools/bootcheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT, qemu_path  # noqa: E402

DISK = os.path.join(ROOT, "bootcheck.%d.img" % os.getpid())
SHOT = os.path.join(ROOT, "build", "bootcheck.ppm")


def lit_band(path, top, bottom):
    """How much is drawn in a band of the screen, in pixels."""
    with open(path, "rb") as f:
        d = f.read()
    parts, i = [], 0
    while len(parts) < 4:
        while i < len(d) and d[i:i + 1].isspace():
            i += 1
        j = i
        while i < len(d) and not d[i:i + 1].isspace():
            i += 1
        parts.append(d[j:i])
    i += 1
    w, h = int(parts[1]), int(parts[2])
    px = d[i:]
    n = 0
    for y in range(top, min(h, bottom)):
        row = px[y * w * 3:(y + 1) * w * 3]
        n += sum(1 for k in range(0, len(row), 3) if row[k] > 40)
    return n


def bare_bios():
    """Hands the disk to a plain BIOS with nothing else attached at all.

    No kernel and no image: this is the boot a person gets on every start
    after the first, and the one that used to end in the firmware executing
    four hundred bytes of nought."""
    if os.path.exists(SHOT):
        os.remove(SHOT)
    proc = subprocess.Popen(
        [qemu_path(), "-drive", "file=%s,format=raw,if=ide,index=0" % DISK,
         "-m", "256", "-display", "none", "-monitor", "stdio"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True)
    told = ""
    try:
        time.sleep(10)
        proc.stdin.write("screendump %s" % SHOT.replace("\\", "/") + chr(10))
        proc.stdin.flush()
        time.sleep(3)
        proc.stdin.write("quit" + chr(10))
        proc.stdin.flush()
        # Read it rather than only waiting on it: the monitor writes back,
        # and a pipe nobody empties is a process that stops when it fills.
        told = proc.communicate(timeout=30)[0] or ""
    except Exception:
        proc.kill()
        told = "(the monitor did not answer)"
    return os.path.exists(SHOT), told


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("starting from the disk")

    if os.path.exists(SHOT):
        os.remove(SHOT)

    # --- a first boot, which formats the disk ------------------------------
    vm = Guest(DISK, size_mb=64, memory=256, keep=True)
    try:
        vm.wait_boot()
        vm.run("ls /", timeout=20)
        # Something to still be there afterwards. The repair rewrites a
        # sector of the volume, so the check that it left the volume alone
        # has to be a file written before it and read after it.
        vm.run("write /keepme.txt this-file-must-survive", timeout=20)
        vm.run("sync", timeout=20)
    finally:
        vm.stop()

    with open(DISK, "rb") as f:
        sec = f.read(512)

    c.add("the disk was formatted", sec[510] == 0x55 and sec[511] == 0xAA)
    c.add("and says which filesystem it is",
          b"FAT16" in sec or b"FAT32" in sec)

    # The jump at the front has to clear the parameter block, and how much
    # of one there is depends on the width.
    wide = b"FAT32" in sec
    want = 0x58 if wide else 0x3C
    c.add("the jump at the front clears the parameters",
          sec[0] == 0xEB and sec[1] == want and sec[2] == 0x90)
    c.add("and lands on an instruction rather than on nought",
          sec[want + 2] != 0)

    # --- and a second boot, from that disk, with nothing else --------------
    got, told = bare_bios()
    c.add("a bios asked to start from it draws something", got)
    if not got:
        print("      qemu said: %s" % told[-300:].replace("\n", " "))

    # Below where the firmware writes "Booting from Hard Disk". Anything
    # drawn there was drawn by the sector, because nothing else has run.
    lit = lit_band(SHOT, 118, 150) if got else 0
    c.add("and the sector's own words are on the screen, so it ran rather "
          "than running off the end", lit > 150)
    if got and lit <= 150:
        print("      %d lit pixels where the sector writes" % lit)

    # --- a disk that was already made wrong --------------------------------
    #
    # Everything above is about a disk this build formatted. Every disk the
    # release before it formatted is still wrong, and shipping the fix does
    # not reach one: a disk that mounts is never formatted again, so the
    # machine goes on dying at the firmware under a kernel that has the fix
    # in it, and the only cure on offer would be erasing the files.
    #
    # So the sector is put back the way that release left it -- the mark,
    # the old jump, and nothing behind the parameters -- and the kernel is
    # booted on it. What is being asked is whether a disk somebody already
    # has is mended by being mounted, and mended rather than emptied.
    code_at = want + 2
    with open(DISK, "r+b") as f:
        sec0 = bytearray(f.read(512))
    for i in range(code_at, 510):
        sec0[i] = 0
    sec0[0], sec0[1], sec0[2] = 0xEB, 0x3C, 0x90       # what it used to write
    with open(DISK, "r+b") as f:
        f.write(bytes(sec0))

    # That the disk really is broken now, before anything is asked to mend
    # it. Without this the two checks below pass on a disk that was never
    # damaged, which is the shape of check that goes green for years.
    with open(DISK, "rb") as f:
        broken = f.read(512)
    c.add("the disk was put back the way the last release left it",
          broken[510] == 0x55 and broken[511] == 0xAA
          and broken[code_at] == 0)

    vm = Guest(DISK, memory=256, keep=True, reuse=True)
    try:
        vm.wait_boot()
        kept = vm.fresh("cat /keepme.txt", timeout=20)
    finally:
        vm.stop()

    with open(DISK, "rb") as f:
        sec = f.read(512)

    c.add("a disk made without a program gets one when it is mounted",
          sec[0] == 0xEB and sec[1] == want and sec[2] == 0x90
          and sec[code_at] != 0)
    c.add("and the files on it are still there",
          "this-file-must-survive" in kept)

    # And the whole of it, the way a person meets it: the machine that would
    # not start, started once with the new kernel, and asked to start from
    # its own disk again.
    got2, told2 = bare_bios()
    c.add("a bios can start from the mended disk", got2)
    if not got2:
        print("      qemu said: %s" % told2[-300:].replace(chr(10), " "))
    lit2 = lit_band(SHOT, 118, 150) if got2 else 0
    c.add("and it stops with a message rather than running off the end",
          lit2 > 150)
    if got2 and lit2 <= 150:
        print("      %d lit pixels after the repair" % lit2)

    if not keep:
        for f in (DISK, SHOT):
            try:
                os.remove(f)
            except OSError:
                pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
