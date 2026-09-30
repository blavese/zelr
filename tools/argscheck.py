"""The words on the kernel's command line, from a kernel file deep in folders.

QEMU hands a multiboot kernel its command line with the name of the file it
loaded in front of the words it was given, and GRUB does the same. The kernel
kept the first 128 bytes of that, so from a checkout deep enough in folders
the path alone filled them: the "console" every harness passes was cut off,
the machine opened its desktop rather than a prompt, and every harness run
from there waited for a prompt that never came. And the kernel looks for its
words anywhere in the line, so a folder called selftest would have started
the self test.

So the kernel is copied into folders like that, one of them called
selftest, and asked for the console. It has to come up at the prompt.

  python tools/argscheck.py
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT, BUILD      # noqa: E402

DISK = os.path.join(ROOT, "argscheck.%d.img" % os.getpid())
TOP = os.path.join(BUILD, "argscheck.%d" % os.getpid())


def deep():
    """Folders under TOP, one called selftest, until the kernel's path is past
    150 characters: longer than the kernel's line, and short of the 260 a
    Windows path may have from however deep a checkout this is run."""
    d = os.path.join(TOP, "selftest")
    while len(os.path.join(d, "zelr.bin")) < 150:
        d = os.path.join(d, "folders-deep")
    return d


def main():
    build_once()
    c = Checks("the kernel's command line")
    where = deep()
    os.makedirs(where, exist_ok=True)
    kernel = os.path.join(where, "zelr.bin")
    shutil.copyfile(os.path.join(BUILD, "zelr.bin"), kernel)
    # Measured, so a shallower checkout cannot make this pass by accident.
    c.add("the kernel file's path is longer than the line the kernel keeps",
          len(kernel) + len(" console") > 128)

    vm = Guest(DISK, memory=128, kernel=kernel)
    try:
        c.add("loaded from folders that deep, one called selftest, it comes up at the prompt it was asked for",
              vm.wait_prompt(1, 120))
    finally:
        vm.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass
        shutil.rmtree(TOP, ignore_errors=True)

    return c.report()


if __name__ == "__main__":
    sys.exit(main())
