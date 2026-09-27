"""The power going out in the middle of a write.

kernel/fat.c makes a specific promise about this, in a comment:

  "before it the old file is live, after it the new one is, and there is no
   moment where neither is"

The write is staged to earn that. The new copy goes into clusters nothing
points at, the data is flushed to the drive before anything points at it,
one sector write swings the directory entry from the old chain to the new,
and only then is the old chain released.

Nothing tested any of it. A comment asserting crash safety is worth nothing
on its own: the failure it describes happens once, on somebody's real disk,
months later, and by then there is no way to tell what it was.

So this switches the machine off while it is writing -- not cleanly, no
poweroff, no flush, the process killed where it stands -- and then boots the
same disk again and looks. Killing QEMU is a fair power cut for this: bytes
the guest actually handed over survive in the host's file, and bytes it
never wrote are gone, which is exactly what a drive does.

The file is rewritten over and over, each copy full of its own generation
number, and the machine says which one it has just finished. Whatever is on
the disk afterwards has to be one copy, whole and the right length, and no
older than the last one the machine said it had finished. Half of one and
half of another is the promise broken. Missing is worse than that. Older
than a write reported done is a disk that was never written.

That last part used to be "both of A and B turn up across the rounds". With a
write taking milliseconds, which of two a random moment lands on is a coin,
and six of them came up the same about one run in thirty two.

The kill lands at a different moment each time, on purpose. A fixed delay
tests one instant of a write that has many, and the interesting ones are the
few milliseconds around the directory entry.

  python tools/crashcheck.py [--keep] [--rounds=N]
"""
import os
import random
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "crashcheck.%d.img" % os.getpid())


def ask(vm, timeout=40):
    """What is on the disk, according to the machine itself: the verdict, the
    generation when it is one whole copy (else None), and what was said."""
    out = vm.fresh("exec /bin/crashwrite check", timeout=timeout)
    m = re.search(r"CRASH_WHOLE (\d+)", out)
    if m:
        return "CRASH_WHOLE", int(m.group(1)), out
    for word in ("CRASH_TORN", "CRASH_SHORT", "CRASH_MISSING"):
        if word in out:
            return word, None, out
    return "CRASH_NOTHING", None, out


def last_turn(text):
    """The last generation the machine said it had finished, or None."""
    turns = re.findall(r"CRASH_TURN (\d+)", text)
    return int(turns[-1]) if turns else None


def main():
    keep = "--keep" in sys.argv
    rounds = 6
    for a in sys.argv[1:]:
        if a.startswith("--rounds="):
            rounds = int(a.split("=", 1)[1])

    build_once()
    c = Checks("the power going out")

    try:
        # --- a file that is definitely there, written cleanly ------------
        vm = Guest(DISK, size_mb=64, memory=256, keep=True)
        try:
            vm.wait_boot()
            out = vm.fresh("exec /bin/crashwrite start", timeout=60)
            c.add("a file can be written", "CRASH_WROTE 0" in out)
            if "CRASH_WROTE 0" not in out:
                print("      got: %r" % out.strip()[-200:])
        finally:
            vm.stop()

        # --- and it is still there on the next boot -----------------------
        vm = Guest(DISK, size_mb=64, memory=256, keep=True, reuse=True)
        try:
            vm.wait_boot()
            verdict, gen, out = ask(vm)
            c.add("and is still there after a clean restart",
                  verdict == "CRASH_WHOLE" and gen == 0)
            if verdict != "CRASH_WHOLE" or gen != 0:
                print("      %s" % out.strip()[-200:])
        finally:
            vm.stop()

        # --- now switch it off while it is writing ------------------------
        survived = []          # (verdict, generation, last one reported done)
        for i in range(rounds):
            vm = Guest(DISK, size_mb=64, memory=256, keep=True, reuse=True)
            try:
                vm.wait_boot()
                vm.type("exec /bin/crashwrite" + chr(10))

                # Wait until it is really writing rather than still starting,
                # so the kill lands in a write and not in the shell.
                started = vm.wait_serial("CRASH_TURN", timeout=60)
                if not started:
                    c.add("round %d: the writing started" % (i + 1), False)
                    continue

                # Somewhere inside one of the rewrites, at a moment this side
                # did not choose.
                time.sleep(random.uniform(0.15, 2.5))

                # What it had said it finished, read before the kill. Anything
                # it said after this and before the power went is not held
                # against it, so this is a floor on what must have survived.
                done = last_turn(vm.serial())
            finally:
                vm.stop()          # terminate, not poweroff: the power going

            vm = Guest(DISK, size_mb=64, memory=256, keep=True, reuse=True)
            try:
                vm.wait_boot()
                verdict, gen, out = ask(vm)
                survived.append((verdict, gen, done))
                if verdict != "CRASH_WHOLE":
                    print("      round %d: %s" % (i + 1, out.strip()[-300:]))
            finally:
                vm.stop()

        whole = [s for s in survived if s[0] == "CRASH_WHOLE"]
        c.add("every power cut leaves one whole version of the file",
              len(whole) == len(survived) and len(survived) == rounds)
        print("      %d of %d rounds: %s"
              % (len(whole), rounds,
                 ", ".join("%s %s after %s" % (v.replace("CRASH_", "").lower(), g, d)
                           for v, g, d in survived)))

        # A machine whose writes never reached the disk leaves an older copy
        # than one it reported finished, every time rather than by chance.
        c.add("and none older than the last write the machine said it had finished",
              len(whole) == rounds and
              all(d is not None and g >= d for _, g, d in whole))

        # --- and the volume is still a volume ------------------------------
        vm = Guest(DISK, size_mb=64, memory=256, keep=True, reuse=True)
        try:
            vm.wait_boot()
            listed = vm.fresh("ls /home", timeout=40)
            c.add("and the volume still mounts and lists", "crash.dat" in listed)
            where = vm.fresh("disk", timeout=40)
            c.add("and is still a filesystem", "fat" in where.lower())
        finally:
            vm.stop()
    finally:
        if not keep:
            try:
                os.remove(DISK)
            except OSError:
                pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
