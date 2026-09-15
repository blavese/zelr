"""Does copy and paste move text between a program and the rest of the system.

The kernel's own checks cover the buffer. What they cannot cover is the path
a person actually uses: a key pressed on real hardware, through the PS/2
driver, through the window manager, into a ring 3 program, and back out
through a system call. Every one of those is a place the control bit can be
dropped, and dropping it looks exactly like a clipboard that does not work.
(Measured: it was dropped, in two places at once. The window manager stripped
every modifier before a program saw it, and the keyboard driver folds
ctrl+letter into a control character, so a program comparing the key against
'c' finds 3 instead and matches nothing.)

So the copying happens in the desktop and the answer is read from
/sys/clipboard on the serial console, which is outside the program that did
it. Nothing here asks a program whether it thinks it worked.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "clipcheck.%d.img" % os.getpid())

SLATE = (0x10, 0x14, 0x1A)
PAGE = (120, 120, 700, 460)

NAMED = {
    " ": "spc", "\n": "ret", "\t": "tab", "/": "slash", ".": "dot",
    "-": "minus", "_": "shift-minus", ",": "comma",
}

MARKER = "clipmarker"
DECOY = "zzzz"


def keys(mon, text, settle=0.06):
    for ch in text:
        mon.send("sendkey %s" % NAMED.get(ch, ch), settle=settle)


def main():
    build_once()
    vm = Guest(DISK, memory=128)
    c = Checks("clipboard test")
    out = ""

    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()

        # The terminal has to be up before anything is typed at it, and how
        # long that takes depends on what else the host is running.
        _, _, _, shot, up = mon.wait_screen(
            "clip-start",
            lambda w, h, px: count_in(px, w, PAGE, SLATE) > 50000, timeout=60)
        c.add("the terminal is up to be typed into", up, shot)

        # 1. Copy the line being typed. With nothing selected that is what
        #    copy takes, which is the case somebody reaches for most.
        keys(mon, MARKER)
        mon.send("sendkey ctrl-c", settle=0.6)

        # 2. Clear it, then paste it back into a command and run it. If the
        #    bytes come out of the clipboard, echo prints them.
        for _ in range(len(MARKER) + 4):
            mon.send("sendkey backspace", settle=0.03)
        keys(mon, "echo ")
        mon.send("sendkey ctrl-v", settle=0.6)
        mon.send("sendkey ret", settle=0.6)

        # 3. Put something else on the clipboard, so what is read at the end
        #    cannot be the original copy still sitting there.
        keys(mon, DECOY)
        mon.send("sendkey ctrl-c", settle=0.6)
        for _ in range(len(DECOY) + 2):
            mon.send("sendkey backspace", settle=0.03)

        # 4. Select the whole scrollback and copy it. The marker can only be
        #    in there if step 2 really pasted and the shell really ran it.
        mon.send("sendkey ctrl-a", settle=0.5)
        mon.send("sendkey ctrl-c", settle=0.6)

        # 5. Leave the desktop and read the clipboard from outside. Waiting
        #    for the console to come back is what tells us the desktop let
        #    go, rather than guessing at how long that takes.
        mon.send("sendkey esc")
        handed_back = vm.wait_serial("back at the shell", timeout=30)
        c.add("the desktop handed the console back", handed_back)

        # The clipboard is read on the console, and what says the read
        # finished is the prompt coming back after it rather than a guess at
        # how long it takes.
        out = vm.run("cat /sys/clipboard")
    finally:
        out = vm.serial() or out
        vm.stop()

    tail = out.split("cat /sys/clipboard")[-1] if "cat /sys/clipboard" in out else ""

    c.add("the clipboard is readable from outside the program",
          bool(tail.strip()))
    c.add("a selection copied out of a ring 3 program reached the kernel",
          MARKER in tail)
    # The last copy was a select-all over the scrollback, so what came back
    # has to be many lines rather than the single input line step 1 copied.
    # Without this the suite would pass on a clipboard that only ever held
    # that first copy, which is exactly what it did when the control bit was
    # being stripped: three of four checks still passed.
    body = tail.split("zelr:")[0]
    c.add("what came back is the scrollback, not the one line first copied",
          body.count("\n") > 3)

    rc = c.report()
    if rc:
        print("\n--- what the console said ---")
        for line in out.splitlines()[-20:]:
            print("   ", line)
    return rc


if __name__ == "__main__":
    sys.exit(main())
