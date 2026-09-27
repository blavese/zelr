"""Typing that arrives all at once.

A person pasting a line into a serial console, or a program driving one,
sends it as fast as the line goes rather than one character at a time. The
16550's receive FIFO is sixteen bytes deep for exactly that, and the kernel
used to set its trigger to one byte -- under which QEMU accepts one byte and
its stdio backend on Windows throws away the rest until the guest has read
it. Measured then: a 26 byte burst reached the interrupt handler as 4 bytes.
It is why every harness here types at one character every fifty
milliseconds, and why a busy host still lost the odd letter from them.

This types whole lines in one write, each short enough for the FIFO, and
asks for them back.

  python tools/serialcheck.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "serialcheck.%d.img" % os.getpid())

# Fourteen bytes each with the newline, the most the FIFO takes before it
# has to be read: what is being checked is that the guest is handed them,
# not how fast it reads.
WORDS = ["qwertyui", "asdfghjk", "zxcvbnm1"]


def burst(vm, text):
    vm.proc.stdin.write(text.encode())
    vm.proc.stdin.flush()


def said(vm, mark, word, timeout=15):
    """Whether the shell printed the word on a line of its own after mark:
    the output of echo, not the command being echoed back."""
    end = time.time() + timeout
    while time.time() < end:
        for line in vm.serial()[mark:].splitlines():
            if line.strip() == word:
                return True
        time.sleep(0.2)
    return False


def main():
    build_once()
    c = Checks("typing that arrives all at once")

    vm = Guest(DISK, memory=128)
    try:
        vm.wait_boot()

        arrived = []
        for word in WORDS:
            n = vm.prompts()
            mark = len(vm.serial())
            burst(vm, "echo %s\n" % word)
            arrived.append(said(vm, mark, word))
            # The next line only once this one is finished with, so each
            # burst meets an empty FIFO and a shell that is reading.
            vm.wait_prompt(n + 1, timeout=20)
        c.add("a line typed all at once arrives whole", arrived[0])
        c.add("and so does the next, and the one after", all(arrived[1:]))

        # The kernel's own count of what the interrupt handler took off the
        # line and what it had to drop, which says the bytes came through
        # the FIFO rather than being retyped by anything.
        out = vm.run("mem", timeout=20)
        got = dropped = -1
        for line in out.splitlines():
            if "serial:" in line:
                for part in line.split():
                    if part.startswith("got="):
                        got = int(part[4:])
                    if part.startswith("dropped="):
                        dropped = int(part[8:])
        c.add("the kernel took every byte and dropped none",
              got >= sum(len(w) + 6 for w in WORDS) and dropped == 0)
        print("      serial got %d, dropped %d" % (got, dropped))
    finally:
        vm.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass

    return c.report()


if __name__ == "__main__":
    sys.exit(main())
