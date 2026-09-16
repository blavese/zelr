"""Files with the names they were given, across a restart and off the disk.

Every file this system saved used to come back as SOMETHI~1.TXT, because a
FAT directory entry holds eight characters and three more and nothing else.
The real name lives in extra entries placed in front of that one, and this
checks they are written, read, survive the machine being turned off, and can
be read by something that is not this kernel.

That last part matters most. A filesystem agreeing with itself proves
nothing. tools/readfat.py parses the volume from the specification, so a name
it can see is really on the disk in the shape the specification describes.

  python tools/namecheck.py
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
DISK = os.path.join(ROOT, "namecheck.%d.img" % os.getpid())

LONG = "a-rather-long-file-name.txt"
DOTS = "two.dots.here.txt"
SHORT = "plain.txt"


def host_listing():
    """What the volume holds, according to the other implementation."""
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "readfat.py"), DISK],
                       capture_output=True, text=True)
    return r.stdout


def main():
    build_once()
    c = Checks("name test")

    vm = Guest(DISK, memory=256, machine="q35", keep=True)
    try:
        vm.wait_boot()

        vm.run("write /home/%s kept" % LONG)
        out = vm.fresh("ls /home")
        c.add("a long name is listed in full", LONG in out)
        c.add("and not cut short", "a-rather-long-fi " not in out)

        out = vm.fresh("cat /home/%s" % LONG)
        c.add("and the file opens under that name", "kept" in out)

        # More than one dot is the other thing eight and three cannot hold.
        vm.run("write /home/%s dots" % DOTS)
        out = vm.fresh("ls /home")
        c.add("a name with several dots survives too", DOTS in out)

        # And a name that always fitted has to keep working exactly as it did,
        # because every volume this system has ever written is full of them.
        vm.run("write /home/%s tiny" % SHORT)
        out = vm.fresh("cat /home/%s" % SHORT)
        c.add("a short name still works", "tiny" in out)
    finally:
        vm.stop()

    # A second machine on the disk the first one wrote, which is the only way
    # to tell a directory entry from a memory of one.
    vm = Guest(DISK, memory=256, machine="q35", reuse=True, keep=True)
    try:
        vm.wait_boot()

        out = vm.fresh("ls /home")
        c.add("the long name is still there on a new machine", LONG in out)

        out = vm.fresh("cat /home/%s" % LONG)
        c.add("and still opens", "kept" in out)

        # Deleting has to take the long entries with it, or a reader that
        # trusts them lists a file that is gone.
        vm.run("rm /home/%s" % DOTS)
        out = vm.fresh("ls /home")
        c.add("a deleted long name disappears", DOTS not in out)
    finally:
        vm.stop()

    listing = host_listing()
    c.add("another implementation sees the long name", LONG in listing)
    c.add("and does not see the deleted one", DOTS not in listing)
    c.add("and still sees the short one", SHORT.upper() in listing.upper())

    try:
        os.remove(DISK)
    except OSError:
        pass
    return c.report()


if __name__ == "__main__":
    sys.exit(main())
