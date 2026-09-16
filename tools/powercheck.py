"""Turning the machine off.

There is no instruction for this. A machine is switched off by asking the
chipset for sleep state five, and what has to be written to ask is two small
numbers the firmware chose and left inside its own bytecode, in an object
called _S5. The kernel finds them by reading that bytecode.

Which means the check is simple and total: tell it to shut down, and see
whether the machine is still there.

  python tools/powercheck.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "powercheck.%d.img" % os.getpid())


def main():
    build_once()
    c = Checks("power test")

    vm = Guest(DISK, memory=256, machine="q35")
    gone = False
    code = None
    log = ""
    try:
        vm.wait_boot()

        # Said once. On a machine where this works the command does not come
        # back, so asking twice is asking a machine that is no longer there.
        vm.type("shutdown" + chr(10))

        for _ in range(80):
            if vm.proc.poll() is not None:
                gone = True
                break
            time.sleep(0.25)
        code = vm.proc.poll()
        log = vm.serial()
    finally:
        vm.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass

    c.add("it knows how to turn the machine off",
          "cannot power off" not in log)
    c.add("the machine stops when told to", gone)
    c.add("and stops cleanly rather than crashing", code == 0)

    # The numbers came out of the firmware's bytecode rather than being
    # guessed, and the log records which ones.
    c.add("the sleep type was read from the firmware", "sleep type" in log)

    return c.report()


if __name__ == "__main__":
    sys.exit(main())
