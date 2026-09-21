"""Programs running on more than one processor.

The other processors used to run a function handed to them and go back to
sleep. That is real parallelism and it is not a processor running anything:
the boot processor owned the scheduler, and a machine given four cores ran
every program on one of them.

This starts several programs that do not stop on their own and then asks the
machine which processor ran what. /sys/cpu counts a slice each time a
processor picks up a program, which is the one number that answers the
question -- a processor that has never run a program has never been a
processor as far as anybody using this machine is concerned.

Run with four, because the fault being checked for is invisible with one.

  python tools/smpcheck.py [--keep]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "smpcheck.%d.img" % os.getpid())

ROW = re.compile(r"cpu(\d+)\s+apic \d+, \w+\s+(\d+) slices, (\d+) ticks, (\d+) busy")


def read_cpus(text):
    """Every processor's line, as (slices, ticks, busy) by index."""
    out = {}
    for m in ROW.finditer(text):
        out[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
    return out


def cpu_table(vm, want):
    """The table, waited for rather than read once.

    A prompt can be recognised while the last line of what was asked for is
    still coming down the serial line, so reading between two prompts on a
    loaded host hands back a table with rows missing -- which reads as a
    machine that has lost a processor."""
    mark = len(vm.serial())
    vm.type("cat /sys/cpu" + chr(10))
    deadline = time.time() + 60
    while time.time() < deadline:
        got = read_cpus(vm.serial()[mark:])
        if len(got) >= want:
            return got
        time.sleep(0.5)
    return read_cpus(vm.serial()[mark:])


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("more than one processor")

    vm = Guest(DISK, memory=256, extra=["-smp", "4"])
    try:
        vm.wait_boot()

        before = cpu_table(vm, 4)
        c.add("the machine describes four processors", len(before) == 4)

        # --- something for them to run ------------------------------------
        #
        # More programs than there are processors, so that leaving one idle
        # is a choice the scheduler made rather than a shortage of work.
        for _ in range(6):
            vm.run("bg /bin/spin", timeout=30)
        time.sleep(6)

        after = cpu_table(vm, 4)
        c.add("and still describes four afterwards", len(after) == 4)

        if len(after) == 4:
            ran = [i for i in after if after[i][0] > before.get(i, (0, 0, 0))[0]]
            c.add("the boot processor ran programs", 0 in ran)
            c.add("and so did at least one other", len([i for i in ran if i]) >= 1)
            if not [i for i in ran if i]:
                print("      slices by processor: %s"
                      % ", ".join("cpu%d %d" % (i, after[i][0]) for i in sorted(after)))

            # --- each one has its own clock -------------------------------
            #
            # Nothing takes a program off a processor without one. The boot
            # processor's tick is the 8254 and is counted elsewhere, so the
            # ones that matter here are the others.
            ticking = [i for i in after if i and after[i][1] > 200]
            c.add("every other processor has a clock of its own",
                  len(ticking) == 3)
            if len(ticking) != 3:
                print("      ticks by processor: %s"
                      % ", ".join("cpu%d %d" % (i, after[i][1]) for i in sorted(after)))

        # --- and the machine is still usable ------------------------------
        #
        # The point of the check that comes last. A scheduler that hands
        # programs to other processors and then wedges the machine has done
        # the interesting half and not the useful one.
        # Waited for by what is said rather than by the prompt coming back.
        #
        # fresh() hands over everything between one prompt and the next, and
        # on a loaded host the prompt can be recognised while the last line
        # of a program's output is still coming down the serial line. That is
        # exactly what happened under the gate: this reported that the
        # machine would not run a program to completion, and the evidence it
        # printed showed the program had run, had printed its first three
        # lines, and printed the fourth a moment later. The machine was fine
        # and the check was reading too early.
        vm.type("ps" + chr(10))
        c.add("and can still be asked what is running",
              vm.wait_serial("spin", timeout=60))

        vm.type("exec /bin/hello" + chr(10))
        c.add("and the machine still runs a program to completion",
              vm.wait_serial("5050", timeout=120))

    finally:
        vm.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
