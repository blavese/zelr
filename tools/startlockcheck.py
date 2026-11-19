"""A machine with a password starts locked, at the console and the desktop.

The password file, /cfg/lock, is written onto a fresh volume here as
Settings writes it (a salt, a space, the hex of SHA-256 over the salt and
the password, from hashlib), and the machine is started on it twice:

  - for the console (the kernel command line's "console", as every
    harness starts it): it asks for the password before anything, takes a
    wrong one for nothing and runs nothing typed before the right one,
    shows what is typed as stars, and then takes commands; the desktop
    opened from there is not locked again;
  - for the desktop (no command line, as a machine is switched on): the
    desktop opens locked, drawn over everything, and only the password
    opens it.

  python tools/startlockcheck.py [--keep]
"""
import hashlib
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT      # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "startlockcheck.%d.img" % os.getpid())
LOCKFILE = os.path.join(ROOT, "startlockcheck.%d.lock" % os.getpid())
SCREEN = (0, 0, 1024, 768)
LOCK_BG = (0x0E, 0x14, 0x1C)              # wm.c LOCK_BG
TERMINAL = (0x10, 0x14, 0x1A)
SALT = "5a17ed0c0ffee123"
PASSWORD = "plum tree 42"


def said_since(vm, mark, text, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if text in vm.serial()[mark:]:
            return True
        time.sleep(0.2)
    return False


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("started locked")
    with open(LOCKFILE, "w", newline="\n") as f:
        f.write(SALT + " " + hashlib.sha256((SALT + PASSWORD).encode()).hexdigest() + "\n")
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768", LOCKFILE + ":cfg/lock"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a password set", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    # --- started for the console ---------------------------------------------
    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        c.add("the console asks for the password before anything",
              said_since(vm, 0, "This machine has a password", 120))
        mark = len(vm.serial())
        vm.type("echo nothing runs\n")
        refused = said_since(vm, mark, "console: that is not the password")
        out = vm.serial()[mark:]
        c.add("a wrong one is refused; what was typed is shown as stars and not run",
              refused and "nothing runs" not in out and "*****" in out)
        mark = len(vm.serial())
        vm.type(PASSWORD + "\n")
        c.add("the password is taken", said_since(vm, mark, "console: the password was given"))
        mark = len(vm.serial())
        vm.type("echo the console is open\n")
        c.add("and then the console takes commands", said_since(vm, mark, "\nthe console is open"))

        mark = len(vm.serial())
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "startlock-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        time.sleep(3)
        c.add("the desktop opened from there is not locked again",
              up and "desktop: locked" not in vm.serial()[mark:], shot)
    finally:
        vm.stop()

    # --- started for the desktop ---------------------------------------------
    vm = Guest(IMAGE, memory=256, reuse=True, keep=True, args="")
    try:
        locked = said_since(vm, 0, "desktop: locked", 120)
        mon = vm.monitor()
        _, _, _, shot, drawn = mon.wait_screen(
            "startlock-locked", lambda w, h, px: count_in(px, w, SCREEN, LOCK_BG) > 400000, timeout=30)
        c.add("started for the desktop, it opens locked, drawn over everything", locked and drawn, shot)
        mark = len(vm.serial())
        vm.type("not it\n")
        c.add("a wrong password does not open it", said_since(vm, mark, "desktop: the password was wrong"))
        mark = len(vm.serial())
        vm.type(PASSWORD + "\n")
        c.add("the password opens it", said_since(vm, mark, "desktop: unlocked"))
    finally:
        vm.stop()

    for junk in (LOCKFILE,) + (() if keep else (IMAGE,)):
        try:
            os.remove(junk)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
