"""A program built outside this repository, run on this machine.

This is the check that says whether zelr can run software it did not write.

Everything else here builds programs from userland/ with the rest of the tree
sitting next to it, which proves nothing about whether sdk/ is enough: a
header that quietly needs a sibling works perfectly until somebody takes it
somewhere else, and then it does not, and the error is about a file nobody
has heard of.

So this copies the four files in sdk/ into a directory outside the tree,
builds there with nothing else in reach, writes the result onto a FAT volume
this repository made rather than one the kernel formatted, boots a machine
with that volume as its disk, and types the program's name.

Which is the whole chain, and every link of it is a place it used to be
impossible:

  the header is enough on its own          -- it was one directory's own code
  the program is built by one command      -- documented, not recalled
  the machine finds it outside /bin        -- both shells looked only there
  it is started on words                   -- exec carried one string
  and it says what it was given

  python tools/sdkcheck.py [--keep]
"""
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

SDK = os.path.join(ROOT, "sdk")
FILES = ("zelr.h", "zelr.ld", "build.sh", "hello.c")

# Where it goes on the volume, which is deliberately not /bin: /bin is the
# directory of programs pasted into the kernel image and does not exist on a
# disk at all.
ON_DISK = "home/outside"


def build_outside(work):
    """The four files, alone in a directory that is not in the tree."""
    for name in FILES:
        shutil.copy(os.path.join(SDK, name), os.path.join(work, name))

    r = subprocess.run(["bash", "build.sh", "hello.c", "outside"],
                       cwd=work, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        return None
    out = os.path.join(work, "outside")
    return out if os.path.exists(out) else None


def make_volume(program, image):
    """A FAT16 volume with the program on it and nothing else.

    Made by tools/mkfat.py rather than by the kernel, so what is mounted is a
    filesystem written by something that only ever read the specification.
    """
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "mkfat.py"),
                        image, "32768", "%s:%s" % (program, ON_DISK)],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-1000:])
        print(r.stderr[-1000:])
        return False
    return True


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("a program from outside")

    work = tempfile.mkdtemp(prefix="zelr-sdk-")
    image = os.path.join(ROOT, "sdkcheck.%d.img" % os.getpid())
    vm = None
    try:
        # --- built with nothing but sdk/ ---------------------------------
        program = build_outside(work)
        c.add("four files out of the tree are enough to build a program",
              program is not None)
        if not program:
            return c.report(keep=keep)

        size = os.path.getsize(program)
        c.add("and what comes out is an executable of a plausible size",
              4096 < size < 200 * 1024)
        print("      %d bytes, built in %s" % (size, work))

        # --- onto a volume this repository wrote -------------------------
        c.add("it can be written onto a FAT volume", make_volume(program, image))

        # --- and run ------------------------------------------------------
        vm = Guest(image, memory=256, reuse=True)
        vm.wait_boot()

        listed = vm.fresh("ls /home", timeout=30)
        c.add("the machine mounts that volume and sees it",
              "outside" in listed)
        if "outside" not in listed:
            print("      ls said: %r" % listed.strip()[-300:])

        # Typed as a name, with words after it. Not `exec /home/outside`:
        # what is being checked is that the machine looks for a program where
        # somebody is standing, and hands over what was typed after it.
        out = vm.fresh("outside alpha beta", timeout=45)
        c.add("typing its name runs it", "hello from outside the tree" in out)
        if "hello from outside the tree" not in out:
            print("      got: %r" % out.strip()[-400:])

        c.add("it ran as a program, with a pid of its own",
              "  pid" in out and "  1..100  5050" in out)

        # argv[0] is what was typed, and the two words after it arrived as
        # two. A machine that glued them back together says [alpha beta].
        c.add("and it was told what it was started on",
              "[outside] [alpha] [beta]" in out)
        if "[outside] [alpha] [beta]" not in out:
            for line in out.splitlines():
                if "argv" in line:
                    print("      argv line: %r" % line.strip())
    finally:
        if vm:
            vm.stop()
        shutil.rmtree(work, ignore_errors=True)
        if not keep:
            try:
                os.remove(image)
            except OSError:
                pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
