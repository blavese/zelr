"""Types into the terminal and checks what it did.

The self test can reach the kernel's side of things, and shotcheck can prove
a window reached the screen. Neither can press a key. This drives QEMU's
monitor to type at the real keyboard, which is the only way to exercise the
line editor, the history and tab completion at all: every one of them lives
in a ring 3 program and is reached only through the PS/2 controller, the
window server's event queue and int 0x80.

Reading text back off a screenshot would need a font reader, so instead every
check is arranged to end in a colour. `theme amber` repaints the terminal in
amber, so a background that turns amber means the whole path worked, and one
that did not means it broke somewhere. That turns tab completion into a
question with a yes or no answer:

    type "them", press Tab, type "amber", press Enter

If completion filled in the rest, the line read `theme amber` and the window
is amber. If it did nothing, the line read `themamber` and nothing happened.

Each step waits for its colour rather than sleeping and hoping. A repaint
that takes four seconds under load is a repaint, not a failure.

  python tools/termcheck.py [--keep]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "termcheck.%d.img" % os.getpid())

# The palettes term.c ships, as the backgrounds they paint.
SLATE = (0x10, 0x14, 0x1A)
PAPER = (0xF2, 0xEE, 0xE4)
AMBER = (0x14, 0x0E, 0x04)
PHOSPHOR = (0x02, 0x0A, 0x02)

# Inside the terminal window, well clear of its chrome and its prompt.
PAGE = (120, 120, 700, 460)

# What the monitor calls the keys that are not letters.
NAMED = {
    " ": "spc", "\n": "ret", "\t": "tab", "/": "slash", ".": "dot",
    "-": "minus", "_": "shift-minus", ",": "comma",
}


def keys(mon, text, settle=0.06):
    """Types a string one key at a time, the way a person would."""
    for ch in text:
        mon.send("sendkey %s" % NAMED.get(ch, ch), settle=settle)


def themed(rgb):
    """True when the terminal's page is painted in this colour. The window is
    most of the screen, so a few thousand pixels is far above any accident."""
    return lambda w, h, px: count_in(px, w, PAGE, rgb) > 50000


def main():
    keep = "--keep" in sys.argv
    build_once()
    vm = Guest(DISK, memory=64)
    c = Checks("terminal test")

    try:
        vm.wait_boot()
        vm.type("desktop\n")       # typed, not pasted: see shell_test.sh
        mon = vm.monitor()

        w, h, px, shot, ok = mon.wait_screen(
            "term-start", themed(SLATE), timeout=60)
        c.add("the terminal opens in its default colours", ok, shot)

        # --- typing at all ------------------------------------------------
        #
        # Nothing has been typed yet, so this is the first proof that a key
        # reaches a ring 3 program: PS/2 controller, kernel keyboard, window
        # server queue, and the program's own event loop.
        keys(mon, "theme paper\n")
        _, _, _, shot, ok = mon.wait_screen("term-typed", themed(PAPER))
        c.add("a typed command reaches a ring 3 program", ok, shot)

        # --- tab completion -----------------------------------------------
        #
        # "them" is not a command. It becomes one only if Tab finishes it.
        keys(mon, "them")
        mon.send("sendkey tab", settle=0.4)
        keys(mon, "amber\n")
        _, _, _, shot, ok = mon.wait_screen("term-completed", themed(AMBER))
        c.add("tab completes a command name", ok, shot)

        # --- history ------------------------------------------------------
        #
        # Two commands back is `theme paper`. Getting there means the up
        # arrow arrived as a key of its own rather than as an escape byte,
        # and that the editor walked the right way through the ring.
        mon.send("sendkey up", settle=0.3)
        mon.send("sendkey up", settle=0.3)
        mon.send("sendkey ret", settle=0.2)
        _, _, _, shot, ok = mon.wait_screen("term-history", themed(PAPER))
        c.add("the up arrow walks back through history", ok, shot)

        # --- editing in the middle of a line ------------------------------
        #
        # Type "theme phosphr", walk the cursor left one, insert the missing
        # letter. Only a real cursor makes this land on a theme that exists.
        keys(mon, "theme phosphr")
        mon.send("sendkey left", settle=0.25)
        keys(mon, "o")
        mon.send("sendkey ret", settle=0.2)
        _, _, _, shot, ok = mon.wait_screen("term-edited", themed(PHOSPHOR))
        c.add("the left arrow moves the cursor, and typing inserts", ok, shot)

        # --- backspace and delete -----------------------------------------
        #
        # "theme paperx", backspace kills the x. Then home, delete, and the
        # line starts one character later: proof that Home and Delete arrive
        # as themselves rather than as text.
        keys(mon, "theme paperx")
        mon.send("sendkey backspace", settle=0.25)
        mon.send("sendkey ret", settle=0.2)
        _, _, _, shot, ok = mon.wait_screen("term-backspace", themed(PAPER))
        c.add("backspace removes the character before the cursor", ok, shot)

        keys(mon, "xtheme amber")
        mon.send("sendkey home", settle=0.25)
        mon.send("sendkey delete", settle=0.25)
        mon.send("sendkey ret", settle=0.2)
        _, _, _, shot, ok = mon.wait_screen("term-delete", themed(AMBER))
        c.add("home goes to the start and delete removes forwards", ok, shot)

        # --- the theme is remembered --------------------------------------
        #
        # It was written to /cfg/term. Reading it back through the terminal's
        # own cat is a round trip through the filesystem from ring 3, and the
        # window is still amber afterwards because the program survived it.
        keys(mon, "cat /cfg/term\n")
        _, _, _, shot, ok = mon.wait_screen("term-cfg", themed(AMBER))
        c.add("the terminal is still running after all of that", ok, shot)
    finally:
        vm.stop()

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
