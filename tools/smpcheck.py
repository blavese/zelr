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
SCREEN = re.compile(r"^(frames|shared|stalled|sentkib) (\d+)", re.M)


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


def screen_table(vm):
    """/sys/screen's counters, waited for the same way: sentkib is the last
    line, so once it is here the rest are."""
    mark = len(vm.serial())
    vm.type("cat /sys/screen" + chr(10))
    deadline = time.time() + 60
    got = {}
    while time.time() < deadline:
        got = dict((k, int(v)) for k, v in SCREEN.findall(vm.serial()[mark:]))
        if "sentkib" in got:
            break
        time.sleep(0.5)
    return got


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("more than one processor")

    vm = Guest(DISK, memory=256, extra=["-smp", "4"])
    try:
        vm.wait_boot()

        before = cpu_table(vm, 4)
        c.add("the machine describes four processors", len(before) == 4)
        screen0 = screen_table(vm)

        # --- something for them to run ------------------------------------
        #
        # More programs than there are processors, so that leaving one idle
        # is a choice the scheduler made rather than a shortage of work.
        for _ in range(6):
            vm.run("bg /bin/spin", timeout=30)
        time.sleep(6)

        after = cpu_table(vm, 4)
        c.add("and still describes four afterwards", len(after) == 4)

        # --- and nothing waits for a processor that is running a program --
        #
        # Every line printed once the console is full scrolls the screen, and
        # every scroll is a flush that hands half its bands to another
        # processor. It used to hand them to one running a program, which
        # never looked, and then spin out the whole wait with interrupts off
        # and the kernel lock held: seconds under emulation, while the serial
        # line dropped whatever was typed. That is how this harness lost
        # "exec /bin/cputest" after everything before it had arrived. The
        # table just printed, with the spinners going, was such a flush.
        screen1 = screen_table(vm)
        c.add("the screen was flushed while every processor was busy",
              screen1.get("frames", 0) > screen0.get("frames", 0))
        c.add("and no flush waited out a processor that was running a program",
              screen1.get("stalled", -1) == 0)
        if screen1.get("stalled", 0):
            print("      %d flushes waited out the other processor"
                  % screen1["stalled"])

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
        #
        # In what came back after ps was typed. This used to wait for "spin"
        # anywhere in the log, and the six "bg /bin/spin" lines above had
        # already said it, so it could not fail.
        mark = len(vm.serial())
        vm.type("ps" + chr(10))
        deadline = time.time() + 60
        while time.time() < deadline and "/bin/spin" not in vm.serial()[mark:]:
            time.sleep(0.2)
        c.add("and can still be asked what is running",
              "/bin/spin" in vm.serial()[mark:])

        vm.type("exec /bin/hello" + chr(10))
        c.add("and the machine still runs a program to completion",
              vm.wait_serial("5050", timeout=120))

        # --- and each program's memory is its own, wherever it runs --------
        #
        # With the spinners still going, so the four workers cputest starts
        # land on whichever processors are free. The kernel used to keep one
        # record of the address space in use for the whole machine, so a
        # program on another processor had its pointers checked against the
        # wrong memory -- invisible with one processor, which is why this is
        # the harness that runs it.
        #
        # Waited for by the whole verdict. This used to wait for "CPUTEST_"
        # and read at once, and under the gate it read "CPUTEST_PA" with
        # every line above it ok, and reported a fault the program never had.
        mark = len(vm.serial())
        vm.type("exec /bin/cputest" + chr(10))
        out = ""
        retyped = False
        deadline = time.time() + 300
        while time.time() < deadline and vm.alive():
            out = vm.serial()[mark:]
            if "CPUTEST_PASS" in out or "CPUTEST_FAIL" in out:
                break
            # A letter of the command lost on the way in, which a busy host's
            # stdio serial does to one character in a few hundred (see
            # harness.type). Typed once more, because what is being checked
            # here is the program; the stall that used to lose whole lines is
            # the check on /sys/screen above.
            if not retyped and ("not a command" in out or "no such file" in out):
                retyped = True
                mark = len(vm.serial())
                vm.type("exec /bin/cputest" + chr(10))
            time.sleep(0.2)
        c.add("every program's memory is its own on every processor",
              "CPUTEST_PASS" in out)
        for line in out.splitlines():
            if "FAIL" in line or "went from" in line or "worker" in line:
                print("      | %s" % line.strip())

    finally:
        vm.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
