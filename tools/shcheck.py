"""The shell, driven the way a person would drive it.

There has been a shell in this system since early on and it lives in the
kernel: every command it has is a function inside the kernel, and it can only
ever do what somebody added to that switch. This one is a program in /bin. It
knows fork, exec and dup2, and everything below is those three arranged
differently.

So what is checked here is not that the commands work. It is that the
arrangements do:

    a redirection is a dup2 between the fork and the exec
    a pipeline is a pipe and two of them
    a built-in is the shell itself, because it changes the shell

Each one is typed at the shell over the serial line and what came back is
read. The programs it runs — echo, cat, ls, wc, grep — are the smallest
things that make an arrangement observable: a pipeline nobody can count the
output of proves nothing.

  python tools/shcheck.py [--keep]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT            # noqa: E402

DISK = os.path.join(ROOT, "shcheck.%d.img" % os.getpid())

# The shell's prompt is the working directory and a dollar. Waiting for it is
# how a command is known to have finished, rather than sleeping for a guess.
PROMPT = re.compile(r"\$ $")

# The same prompt as the last line of a command's output, which is where it
# always is and which is not part of what the command said.
PROMPT_LINE = re.compile(r"\r?\n[^\r\n]*\$ ?$")


def sh(vm, line, timeout=25.0):
    """Types a line at the shell and returns everything it said back."""
    before = len(vm.serial())
    vm.type(line + "\n")
    end = time.time() + timeout
    while time.time() < end:
        out = vm.serial()[before:]
        if PROMPT.search(out):
            return out
        time.sleep(0.2)
    return vm.serial()[before:]


def body(out, line):
    """What a command printed, without the echo of the line that asked for it
    or the prompt that followed.

    Both ends have to come off. The console echoes what is typed, so the
    command is always the first thing in its own output; and the shell prints
    its prompt straight afterwards, so the last line of the output is a
    prompt rather than anything the command said. Leaving it on made `pwd` in
    /bin come back as the directory followed by the prompt, which ends in a
    dollar rather than in a directory, and read as a cd that had not worked.
    """
    text = out
    at = text.find(line)
    if at >= 0:
        text = text[at + len(line):]
    text = PROMPT_LINE.sub("", text)
    return text.strip()


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the shell")

    vm = Guest(DISK, memory=256)
    try:
        vm.wait_boot()

        # --- it starts -------------------------------------------------------
        before = len(vm.serial())
        vm.type("exec /bin/sh\n")
        end = time.time() + 30
        started = False
        while time.time() < end:
            if "zelr shell" in vm.serial()[before:]:
                started = True
                break
            time.sleep(0.3)
        c.add("a shell in ring 3 starts", started)
        if not started:
            return c.report(keep=keep)

        # --- running a program ------------------------------------------------
        out = sh(vm, "echo hello")
        c.add("it runs a program from /bin", "hello" in body(out, "echo hello"))

        out = sh(vm, "nosuchprogram")
        c.add("and says so when there is no such program",
              "not found" in out)

        # --- redirection ------------------------------------------------------
        #
        # The output of a program that knows nothing about files ends up in
        # one. Nothing was recompiled to make this work: echo writes to
        # descriptor 1 as it always did, and the shell changed what 1 is.
        sh(vm, "echo one > /t.txt")
        out = sh(vm, "cat /t.txt")
        c.add("a program's output can be sent to a file",
              "one" in body(out, "cat /t.txt"))

        sh(vm, "echo two >> /t.txt")
        out = sh(vm, "wc /t.txt")
        counts = body(out, "wc /t.txt").split()
        c.add("and added to the end of one",
              len(counts) >= 3 and counts[0] == "2" and counts[1] == "2")

        # And the shell itself is still talking to the screen afterwards,
        # which it would not be if it had redirected its own descriptor
        # rather than the child's.
        out = sh(vm, "echo back")
        c.add("and the shell is still printing to the screen afterwards",
              "back" in body(out, "echo back"))

        # --- taking input from a file ----------------------------------------
        out = sh(vm, "wc < /t.txt")
        counts = body(out, "wc < /t.txt").split()
        c.add("a program's input can come from a file",
              len(counts) >= 3 and counts[0] == "2")

        # --- a pipeline -------------------------------------------------------
        out = sh(vm, "cat /t.txt | grep two")
        said = body(out, "cat /t.txt | grep two")
        c.add("one program's output becomes another's input", "two" in said)
        c.add("and what it filtered out did not come through", "one" not in said)

        out = sh(vm, "ls /bin | wc", timeout=40)
        counts = body(out, "ls /bin | wc").split()
        c.add("a pipeline counts what came down it",
              len(counts) >= 3 and counts[0].isdigit() and int(counts[0]) >= 15)

        # Three of them, because two is the case where the shell only ever
        # has to hold one end at a time.
        out = sh(vm, "ls /bin | grep sh | wc", timeout=40)
        counts = body(out, "ls /bin | grep sh | wc").split()
        c.add("and three of them is no different from two",
              len(counts) >= 3 and counts[0].isdigit() and int(counts[0]) >= 1)

        # --- stopping something -----------------------------------------------
        #
        # The thing a shell is unusable without. /bin/spin does not stop on
        # its own and does not sleep, so nothing but an interruption ends it:
        # before signals existed, starting it was how you lost the machine.
        #
        # What is being checked is three things at once. The program stops.
        # The shell does not — it asked for that interrupt to be ignored, so
        # what reaches it is the prompt and not the end of the session. And
        # the shell is still working afterwards, which is the one that would
        # catch an interrupt that left the wait it was in unfinished.
        before = len(vm.serial())
        vm.type("spin\n")
        end = time.time() + 20
        started = False
        while time.time() < end:
            if "spinning" in vm.serial()[before:]:
                started = True
                break
            time.sleep(0.3)
        c.add("a program that never stops on its own can be started", started)

        # The interrupt itself, as the byte a terminal sends.
        before = len(vm.serial())
        vm.type(chr(3))
        end = time.time() + 20
        back = False
        while time.time() < end:
            if PROMPT.search(vm.serial()[before:]):
                back = True
                break
            time.sleep(0.3)
        c.add("and interrupting it hands the shell back", back)

        out = sh(vm, "echo still here")
        c.add("and the shell is still the shell afterwards",
              "still here" in body(out, "echo still here"))

        out = sh(vm, "ps | grep spin", timeout=30)
        c.add("and the program it stopped is not running any more",
              "spin" not in body(out, "ps | grep spin"))

        # --- the built ins ----------------------------------------------------
        #
        # These have to be the shell rather than programs. A cd that ran as a
        # child would change the child's directory and then the child would
        # exit, and the shell would be exactly where it was.
        out = sh(vm, "pwd")
        c.add("it says where it is", body(out, "pwd") == "/home")

        sh(vm, "cd /bin")
        out = sh(vm, "pwd")
        c.add("and going somewhere else changes where it is",
              body(out, "pwd").strip().endswith("/bin"))

        out = sh(vm, "ls | grep echo", timeout=30)
        c.add("and the directory it moved to is the one programs see",
              "echo" in body(out, "ls | grep echo"))

        sh(vm, "cd /")

        # --- and it leaves ----------------------------------------------------
        before = len(vm.serial())
        vm.type("exit\n")
        end = time.time() + 20
        left = False
        while time.time() < end:
            if re.search(r"zelr(?::\S*)?> ", vm.serial()[before:]):
                left = True
                break
            time.sleep(0.3)
        c.add("and leaving it comes back to the kernel's own shell", left)
    finally:
        vm.stop()
        if not keep and os.path.exists(DISK):
            os.remove(DISK)

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
