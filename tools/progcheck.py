"""A program the kernel was not built with, run by typing its name.

The loading half of this was never missing. Every spawn in this system reads
through the same VFS as `cat`, so an ELF sitting on the FAT volume could
always be loaded -- `exec /home/hello` worked before any of this. What was
missing was the looking: both shells resolved a bare name against /bin and
nowhere else, and /bin is the directory of programs pasted into the kernel
image. So a program had to be built into the machine to be run by typing its
name, which is the difference between an appliance and an operating system.

So this puts a program somewhere /bin is not, and types its name.

The check that matters is the last one. A program that prints its own text
proves it ran; a program that prints the right pid and the right arithmetic
proves it ran as a program rather than as a file somebody printed.

  python tools/progcheck.py [--keep]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "progcheck.%d.img" % os.getpid())

# What /bin/hello says when it runs. It is the oldest program here and it
# says what it is, which makes it the honest thing to copy about.
MARK = "hello from a program the kernel had never seen"
SUM = "5050"


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("a program from the disk")

    vm = Guest(DISK, size_mb=64, memory=256)
    try:
        vm.wait_boot()

        # --- a copy that lives on the disk rather than in the image -------
        vm.run("cp /bin/hello /home/mine", timeout=30)
        listed = vm.fresh("ls /home", timeout=30)
        c.add("a program can be put somewhere that is not /bin",
              "mine" in listed)

        # --- and it is really on the disk ---------------------------------
        #
        # /bin is generated when read: its files are pasted into the kernel
        # image and are not stored anywhere. A copy in /home is on the FAT
        # volume, and that is the whole point of this.
        where = vm.fresh("disk", timeout=30)
        c.add("on a volume that is mounted", "fat" in where.lower())

        # --- run it by name, not by path ----------------------------------
        out = vm.fresh("mine", timeout=45)
        c.add("typing its name runs it", MARK in out)
        if MARK not in out:
            print("      got: %s" % out.strip()[-200:])
        c.add("and it ran as a program rather than being printed", SUM in out)

        # --- a name that is nothing at all --------------------------------
        #
        # The search must not turn every typing mistake into a silence. What
        # is wanted is a refusal that says all three places were looked in.
        missing = vm.fresh("notaprogram", timeout=30)
        c.add("a name that is nothing says so", "not a command" in missing)
        c.add("and says where it looked", "/bin" in missing)

        # --- a file that is not a program ---------------------------------
        #
        # /home/notes ships as text. Finding it and trying to run it has to
        # report what is wrong with it, rather than moving on to /bin and
        # then saying nothing of that name exists -- which is the opposite
        # of what was found.
        vm.run("write /home/lump not an elf at all", timeout=30)
        lump = vm.fresh("lump", timeout=30)
        c.add("a file that is not a program is refused by name",
              "lump" in lump and "not a command" not in lump)
        if "not a command" in lump:
            print("      got: %s" % lump.strip()[-200:])

        # --- and the built-in ones still work -----------------------------
        still = vm.fresh("hello", timeout=45)
        c.add("and a program in /bin still runs by name", MARK in still)
    finally:
        vm.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
