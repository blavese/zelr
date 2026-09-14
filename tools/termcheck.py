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

  python tools/termcheck.py [--keep]
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
DISK = os.path.join(ROOT, "termcheck.img")
MONITOR_PORT = 55733

QEMU = os.environ.get("QEMU") or "C:/Program Files/qemu/qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    from shutil import which
    QEMU = which("qemu-system-x86_64") or QEMU

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shotcheck import Monitor, count_in, tally      # noqa: E402

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


def background(mon, name, shots):
    w, h, px, ppm = mon.screen(name)
    shots.append(ppm)
    return w, px


def is_theme(px, w, rgb):
    """True when the terminal's page is painted in this colour. The window is
    most of the screen, so a few thousand pixels is far above any accident."""
    return count_in(px, w, PAGE, rgb) > 50000


def main():
    keep = "--keep" in sys.argv
    # Built here when this is run on its own, and not when the gate runs
    # it. The gate builds once and then starts several harnesses at the
    # same time; a second build rewrites build/zelr.bin and build/zelr.elf
    # underneath whichever machine is reading them, which on Windows is a
    # permission error rather than a torn file.
    if os.environ.get("ZELR_PREBUILT") != "1":
        subprocess.run(["bash", "build.sh"], cwd=ROOT, check=True,
                       stdout=subprocess.DEVNULL)

    # A fresh disk, so a theme left by an earlier run cannot make a check
    # pass before anything has been typed.
    if os.path.exists(DISK):
        os.remove(DISK)
    with open(DISK, "wb") as f:
        f.truncate(32 * 1024 * 1024)

    proc = subprocess.Popen(
        [QEMU, "-kernel", os.path.join(BUILD, "zelr.bin"), "-m", "64",
         "-no-reboot", "-display", "none", "-serial", "stdio",
         "-drive", "file=%s,format=raw,if=ide,index=0" % DISK,
         "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONITOR_PORT],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT)

    shots = []
    checks = []

    try:
        time.sleep(4.5)                        # let it boot
        for ch in "desktop\n":                 # typed, not pasted: see shell_test.sh
            proc.stdin.write(ch.encode())
            proc.stdin.flush()
            time.sleep(0.05)
        time.sleep(5.0)                        # the terminal starts and draws

        mon = Monitor(MONITOR_PORT)

        w, px = background(mon, "term-start", shots)
        checks.append(("the terminal opens in its default colours",
                       is_theme(px, w, SLATE)))

        # --- typing at all ------------------------------------------------
        #
        # Nothing has been typed yet, so this is the first proof that a key
        # reaches a ring 3 program: PS/2 controller, kernel keyboard, window
        # server queue, and the program's own event loop.
        keys(mon, "theme paper\n")
        time.sleep(3.0)
        w, px = background(mon, "term-typed", shots)
        checks.append(("a typed command reaches a ring 3 program",
                       is_theme(px, w, PAPER)))

        # --- tab completion -----------------------------------------------
        #
        # "them" is not a command. It becomes one only if Tab finishes it.
        keys(mon, "them")
        mon.send("sendkey tab", settle=0.5)
        keys(mon, "amber\n")
        time.sleep(3.0)
        w, px = background(mon, "term-completed", shots)
        checks.append(("tab completes a command name", is_theme(px, w, AMBER)))

        # --- history ------------------------------------------------------
        #
        # Two commands back is `theme paper`. Getting there means the up
        # arrow arrived as a key of its own rather than as an escape byte,
        # and that the editor walked the right way through the ring.
        mon.send("sendkey up", settle=0.35)
        mon.send("sendkey up", settle=0.35)
        mon.send("sendkey ret", settle=0.35)
        time.sleep(3.0)
        w, px = background(mon, "term-history", shots)
        checks.append(("the up arrow walks back through history",
                       is_theme(px, w, PAPER)))

        # --- editing in the middle of a line ------------------------------
        #
        # Type "theme phosphr", walk the cursor left one, insert the missing
        # letter. Only a real cursor makes this land on a theme that exists.
        keys(mon, "theme phosphr")
        mon.send("sendkey left", settle=0.3)
        keys(mon, "o")
        mon.send("sendkey ret", settle=0.35)
        time.sleep(3.0)
        w, px = background(mon, "term-edited", shots)
        checks.append(("the left arrow moves the cursor, and typing inserts",
                       is_theme(px, w, PHOSPHOR)))

        # --- backspace and delete -----------------------------------------
        #
        # "theme paperX", backspace kills the X. Then home, delete, and a
        # fresh letter: proof that Home and Delete arrive as themselves.
        keys(mon, "theme paperx")
        mon.send("sendkey backspace", settle=0.3)
        mon.send("sendkey ret", settle=0.35)
        time.sleep(3.0)
        w, px = background(mon, "term-backspace", shots)
        checks.append(("backspace removes the character before the cursor",
                       is_theme(px, w, PAPER)))

        keys(mon, "xtheme amber")
        mon.send("sendkey home", settle=0.3)
        mon.send("sendkey delete", settle=0.3)
        mon.send("sendkey ret", settle=0.35)
        time.sleep(3.0)
        w, px = background(mon, "term-delete", shots)
        checks.append(("home goes to the start and delete removes forwards",
                       is_theme(px, w, AMBER)))

        # --- the theme is remembered --------------------------------------
        #
        # It was written to /cfg/term. Reading it back through the terminal's
        # own cat is a round trip through the filesystem from ring 3.
        keys(mon, "cat /cfg/term\n")
        time.sleep(3.0)
        w, px = background(mon, "term-cfg", shots)
        checks.append(("the terminal is still running after all of that",
                       is_theme(px, w, AMBER)))

        mon.close()

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        if not keep and os.path.exists(DISK):
            os.remove(DISK)

    print("=== terminal test ===")
    failed = 0
    for name, passed in checks:
        print("  %s  %s" % ("PASS" if passed else "FAIL", name))
        if not passed:
            failed += 1
    print()
    if failed:
        print("terminal test: %d failed" % failed)
        print("screenshots: %s" % ", ".join(shots))
    else:
        print("terminal test: all %d checks passed" % len(checks))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
