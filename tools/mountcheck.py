"""A USB stick with files on it, mounted and used.

Reading a stick a sector at a time proved the driver worked and proved
nothing about whether it was any use: the block layer held exactly one disk,
picked at boot, so the filesystem had no way to ask for any other one. The
stick was a device you could dump hex out of.

This puts a real FAT volume on a stick, plugs it in, and then does the thing
a person would actually do with it: list it, read a file off it, copy a file
onto it, and copy one between it and the disk the machine booted from. The
last check reads the image back on this side with tools/readfat.py, which
parses FAT from the specification rather than asking the kernel, so a file
that is there is really there.

  python tools/mountcheck.py
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
STICK = os.path.join(ROOT, "mountcheck.%d.img" % os.getpid())
DISK = os.path.join(ROOT, "mountdisk.%d.img" % os.getpid())
SEED = os.path.join(ROOT, "mountseed.%d.txt" % os.getpid())

ON_STICK = "a file that was already on the stick"
FROM_ZELR = "written from inside the machine"

USB = ["-device", "qemu-xhci,id=xhci",
       "-drive", "if=none,id=stick,format=raw,file=" + STICK.replace("\\", "/"),
       "-device", "usb-storage,bus=xhci.0,drive=stick"]


def make_stick():
    """A FAT volume with one file on it, built by our own tool."""
    with open(SEED, "w", encoding="ascii", newline="") as f:
        f.write(ON_STICK)

    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"),
                        STICK, "8192", SEED + ":HELLO.TXT"],
                       capture_output=True, text=True)
    return r.returncode == 0


def on_stick_now():
    """What the stick holds, read on this side rather than asked for."""
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "readfat.py"), STICK],
                       capture_output=True, text=True)
    return r.stdout


def file_on_stick(name):
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "readfat.py"),
                        STICK, name], capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else ""


def main():
    build_once()
    c = Checks("mount test")

    if not make_stick():
        c.add("a fat volume can be built for the stick", False)
        return c.report()

    vm = Guest(DISK, memory=256, machine="q35", extra=USB)
    try:
        vm.wait_boot()
        boot = vm.serial()
        c.add("the stick is found and mounted", "usb volume mounted" in boot)

        # fresh rather than run throughout: run hands back the whole
        # console, and the command being echoed contains the very text these
        # are looking for, so they would pass with nothing mounted at all.
        # Two of them did, until a deliberate break showed it.
        out = vm.fresh("ls /usb")
        c.add("its files are listed", "HELLO.TXT" in out.upper())

        out = vm.fresh("cat /usb/HELLO.TXT")
        c.add("and one of them can be read", ON_STICK in out)

        # Onto the stick, which is the direction that has to reach the device.
        #
        # Checked by listing rather than by reading it back, because reading
        # it back proves nothing: with nothing mounted, /usb falls through to
        # the in-memory filesystem and the file is written and read there
        # quite happily. Seeing it next to the file that was already on the
        # volume is what cannot happen unless the volume is really there.
        vm.run("write /usb/MADE.TXT %s" % FROM_ZELR.replace(" ", "_"))
        out = vm.fresh("ls /usb").upper()
        c.add("a file written to it joins the one already there",
              "MADE.TXT" in out and "HELLO.TXT" in out)

        # And between the two volumes, which is the whole point of there
        # being more than one.
        vm.run("cp /usb/HELLO.TXT /home/COPIED.TXT")
        out = vm.fresh("cat /home/COPIED.TXT")
        c.add("a file copies from the stick to the disk", ON_STICK in out)

        out = vm.fresh("ls /")
        c.add("the root shows usb alongside bin and sys", "usb/" in out)
    finally:
        vm.stop()

    # The strongest of them: the bytes are in the image, and something that
    # is not this kernel says so.
    listing = on_stick_now()
    c.add("the written file is really on the stick", "MADE.TXT" in listing.upper())
    c.add("and its contents are what was written",
          "written_from_inside" in file_on_stick("MADE.TXT"))

    for junk in (STICK, DISK, SEED):
        try:
            os.remove(junk)
        except OSError:
            pass
    return c.report()


if __name__ == "__main__":
    sys.exit(main())
