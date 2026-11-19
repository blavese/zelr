"""The screen lock: a password set in Settings, alt+L, and nothing but the
password opens it.

The desktop says on the console when it locks and opens ("desktop:
locked", "desktop: the password was wrong", "desktop: unlocked"), and
Settings' Lock page what it did ("settings: lock password set"); the
password file, /cfg/lock, is read back off the disk image and its hash
checked here with hashlib. Settings is opened from the launcher with the
terminal closed first, so it is the first window, where setcheck's numbers
put it, and its Lock page is clicked there.

  - Settings' Lock page sets a password: /cfg/lock is a salt and the hex of
    SHA-256 over the salt and the password, and nothing of the password;
  - alt+L locks the screen, and the lock is drawn over everything;
  - a wrong password does not open it;
  - alt+Escape, which leaves the desktop, does not get round it;
  - nor does the pointer: the Trash icon double clicked under the lock
    opens nothing (Files would say "files: /home/trash");
  - the right password opens it;
  - Lock the screen in the launcher locks it too;
  - with the password taken away in Settings, Return alone opens it.

  python tools/lockcheck.py [--keep]
"""
import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT              # noqa: E402
from readfat import Fat16                                                 # noqa: E402
from setcheck import (sidebar, PAGE_X, IN_Y, CTX_X, CTX_Y, CTX_PAD,        # noqa: E402
                      CTX_ITEM, CTX_CLOSE, DOCK_SIDE, PANEL_Y, DOCK_H)
from deskcheck import desk_icon_rect                                      # noqa: E402

DISK = os.path.join(ROOT, "lockcheck.%d.img" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
LOCK_BG = (0x0E, 0x14, 0x1C)              # wm.c LOCK_BG
PASSWORD = "zelr123"

# settings.c's Lock page: the heading (28 + 6) and a line (22 + 6) under
# UI_PAD, then the field (240 wide) and its two buttons (90 and 110), 30 high.
ROW_Y = IN_Y + 8 + 34 + 28 + 15
FIELD = (PAGE_X + 120, ROW_Y)
SET = (PAGE_X + 240 + 6 + 45, ROW_Y)
TAKE_AWAY = (PAGE_X + 240 + 6 + 90 + 6 + 55, ROW_Y)


def read(path):
    try:
        return Fat16(DISK).read_file(path)
    except Exception:
        return None


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the screen lock")
    vm = Guest(DISK, memory=256, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "lock-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(2)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def type_(text):
            for ch in text:
                key("spc" if ch == " " else ch, settle=0.06)

        def said_since(mark, text, timeout=20):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        def launcher(word):
            mon.click(DOCK_SIDE + 16 + 38, PANEL_Y + DOCK_H // 2)
            time.sleep(1.5)
            type_(word)
            key("ret", settle=0.5)

        def lock_shown(on, name):
            return mon.wait_screen(
                name, lambda w, h, px: (count_in(px, w, SCREEN, LOCK_BG) > 400000) == on, timeout=20)

        # The terminal away, then Settings, its Lock page.
        mon.move_to(950, 200)
        mon.send("mouse_button 2", settle=0.4)
        mon.send("mouse_button 0", settle=1.2)
        mon.click(CTX_X + 60, CTX_Y + CTX_PAD + CTX_CLOSE * CTX_ITEM + CTX_ITEM // 2)
        time.sleep(2.5)

        def lock_page():
            mark = len(vm.serial())
            launcher("sett")
            time.sleep(4)
            mon.click(*sidebar("Lock"))
            return said_since(mark, "settings: lock ", 20)

        page = lock_page()
        mark = len(vm.serial())
        mon.click(*FIELD)
        type_(PASSWORD)
        mon.click(*SET)
        set_ = said_since(mark, "settings: lock password set")
        line = (read("/cfg/lock") or b"").decode("ascii", "replace").strip()
        salt, _, hexd = line.partition(" ")
        right = hashlib.sha256((salt + PASSWORD).encode()).hexdigest()
        c.add("Settings' Lock page sets a password: a salt and SHA-256 over it and the password (%s)" % line[:24],
              page and set_ and len(salt) == 16 and hexd == right and PASSWORD not in line)
        key("alt-q", settle=1.0)
        time.sleep(1.0)

        mark = len(vm.serial())
        key("alt-l")
        locked = said_since(mark, "desktop: locked")
        _, _, _, shot, shown = lock_shown(True, "lock-on")
        c.add("alt+L locks the screen, the lock over everything", locked and shown, shot)

        mark = len(vm.serial())
        type_("wrong")
        key("ret")
        wrong = said_since(mark, "desktop: the password was wrong")
        _, _, _, shot, still = lock_shown(True, "lock-wrong")
        c.add("a wrong password does not open it", wrong and still and "desktop: unlocked" not in vm.serial()[mark:], shot)

        mark = len(vm.serial())
        key("alt-esc")
        time.sleep(3.0)
        _, _, _, shot, still = lock_shown(True, "lock-alt-esc")
        c.add("alt+Escape does not leave the desktop round it",
              still and "back at the shell" not in vm.serial()[mark:], shot)

        x0, y0, x1, y1 = desk_icon_rect(5)
        mark = len(vm.serial())
        mon.move_to((x0 + x1) // 2, y0 + 30)
        for _ in range(2):
            mon.send("mouse_button 1", settle=0.12)
            mon.send("mouse_button 0", settle=0.12)
        time.sleep(4.0)
        c.add("nor does the pointer: the Trash icon double clicked under the lock opens nothing",
              "files: /home/trash" not in vm.serial()[mark:])

        mark = len(vm.serial())
        type_(PASSWORD)
        key("ret")
        opened = said_since(mark, "desktop: unlocked")
        _, _, _, shot, gone = lock_shown(False, "lock-off")
        c.add("the right password opens it", opened and gone, shot)

        mark = len(vm.serial())
        launcher("lock the")         # "lock" alone finds the Clock first
        by_launcher = said_since(mark, "desktop: locked")
        type_(PASSWORD)
        key("ret")
        c.add("Lock the screen in the launcher locks it too", by_launcher and said_since(mark, "desktop: unlocked"))
        lock_shown(False, "lock-off2")

        page = lock_page()
        mark = len(vm.serial())
        mon.click(*TAKE_AWAY)
        taken = said_since(mark, "settings: lock password taken away")
        key("alt-q", settle=1.0)
        time.sleep(1.0)
        mark = len(vm.serial())
        key("alt-l")
        said_since(mark, "desktop: locked")
        key("ret")
        c.add("with the password taken away, Return alone opens it",
              page and taken and read("/cfg/lock") is None and said_since(mark, "desktop: unlocked"))
    finally:
        vm.stop()
        if not keep and os.path.exists(DISK):
            os.remove(DISK)
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
