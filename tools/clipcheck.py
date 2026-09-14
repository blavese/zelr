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
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from shotcheck import Monitor, QEMU, BUILD          # noqa: E402

PORT = 45613
DISK = os.path.join(ROOT, "clipcheck.%d.img" % os.getpid())

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
    if os.path.exists(DISK):
        os.remove(DISK)
    with open(DISK, "wb") as f:
        f.truncate(32 * 1024 * 1024)

    proc = subprocess.Popen(
        [QEMU, "-kernel", os.path.join(BUILD, "nyx.bin"), "-m", "128",
         "-no-reboot", "-display", "none", "-serial", "stdio",
         "-drive", "file=%s,format=raw,if=ide,index=0" % DISK,
         "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT)

    fails = 0

    def check(name, ok):
        nonlocal fails
        print("  %s  %s" % ("PASS" if ok else "FAIL", name))
        if not ok:
            fails += 1

    out = ""
    try:
        time.sleep(5.0)
        for ch in "desktop\n":
            proc.stdin.write(ch.encode()); proc.stdin.flush(); time.sleep(0.05)
        time.sleep(6.5)

        mon = Monitor(PORT)

        # 1. Copy the line being typed. With nothing selected that is what
        #    copy takes, which is the case somebody reaches for most.
        keys(mon, MARKER)
        time.sleep(0.4)
        mon.send("sendkey ctrl-c", settle=0.8)

        # 2. Clear it, then paste it back into a command and run it. If the
        #    bytes come out of the clipboard, echo prints them.
        for _ in range(len(MARKER) + 4):
            mon.send("sendkey backspace", settle=0.03)
        keys(mon, "echo ")
        mon.send("sendkey ctrl-v", settle=0.8)
        mon.send("sendkey ret", settle=0.8)
        time.sleep(1.0)

        # 3. Put something else on the clipboard, so what is read at the end
        #    cannot be the original copy still sitting there.
        keys(mon, DECOY)
        time.sleep(0.3)
        mon.send("sendkey ctrl-c", settle=0.8)
        for _ in range(len(DECOY) + 2):
            mon.send("sendkey backspace", settle=0.03)

        # 4. Select the whole scrollback and copy it. The marker can only be
        #    in there if step 2 really pasted and the shell really ran it.
        mon.send("sendkey ctrl-a", settle=0.6)
        mon.send("sendkey ctrl-c", settle=0.8)

        # 5. Leave the desktop and read the clipboard from outside.
        mon.send("sendkey esc", settle=1.5)
        for ch in "cat /sys/clipboard\n":
            proc.stdin.write(ch.encode()); proc.stdin.flush(); time.sleep(0.05)
        time.sleep(2.5)
    finally:
        proc.kill()
        out = proc.stdout.read().decode("utf-8", "replace")

    tail = out.split("cat /sys/clipboard")[-1] if "cat /sys/clipboard" in out else ""

    check("the desktop handed the console back", "cat /sys/clipboard" in out)
    check("the clipboard is readable from outside the program", bool(tail.strip()))
    check("a selection copied out of a ring 3 program reached the kernel",
          MARKER in tail)
    # The last copy was a select-all over the scrollback, so what came back
    # has to be many lines rather than the single input line step 1 copied.
    # Without this the suite would pass on a clipboard that only ever held
    # that first copy, which is exactly what it did when the control bit was
    # being stripped: three of four checks still passed.
    body = tail.split("nyx:")[0]
    check("what came back is the scrollback, not the one line first copied",
          body.count("\n") > 3)

    if fails:
        print("\n--- what the console said ---")
        for line in out.splitlines()[-20:]:
            print("   ", line)

    if os.path.exists(DISK):
        try:
            os.remove(DISK)
        except OSError:
            pass
    print("\n%s" % ("all checks passed" if fails == 0 else "%d failed" % fails))
    return 1 if fails else 0


sys.exit(main())
