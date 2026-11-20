"""alt+Print Screen saves the front window alone.

The terminal is the window in front, its page one colour. alt+Print Screen
reaches the machine as a PS/2 keyboard sends it, as SysRq. The desktop says
on the console where it saved the picture, which is read off the disk and
decoded here by printcheck's reader.

  - it is saved, named "window" and the time;
  - it is the window's size, smaller than the screen;
  - and it holds the window: as much of the terminal's page as the screen
    shows, and nothing much else;
  - with every window put away, it says there is none in front.

  python tools/windowshotcheck.py [--keep]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402
from printcheck import png_pixels                            # noqa: E402

DISK = os.path.join(ROOT, "windowshotcheck.%d.img" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)


def count_rgb(rgb, colour):
    want = bytes(colour)
    return sum(1 for i in range(0, len(rgb), 3) if rgb[i:i + 3] == want)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the front window saved")
    vm = Guest(DISK, memory=256, keep=True)
    path = None
    on_screen = 0
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "wshot-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal in front", up, shot)
        time.sleep(2)
        w, h, px, _, _ = mon.wait_screen("wshot-before", lambda w, h, px: True)
        on_screen = count_in(px, w, SCREEN, TERMINAL)

        mark = len(vm.serial())
        mon.send("sendkey alt-print", settle=0.3)
        end = time.time() + 60
        while time.time() < end and not path:
            m = re.search(r"desktop: the window is saved in (\S.*\.png)", vm.serial()[mark:])
            path = m.group(1).strip() if m else None
            time.sleep(0.2)
        c.add("alt+Print Screen saves it, named for a window (%s)" % path,
              path is not None and "/home/pictures/window " in path)

        mon.send("sendkey alt-d", settle=1.0)
        mark = len(vm.serial())
        mon.send("sendkey alt-print", settle=0.3)
        end = time.time() + 20
        none = False
        while time.time() < end and not none:
            none = "desktop: the window was not saved: there is no window in front" in vm.serial()[mark:]
            time.sleep(0.2)
        c.add("with every window put away, it says there is none in front", none)
    finally:
        vm.stop()

    got = None
    if path:
        try:
            got = png_pixels(Fat16(DISK).read_file(path))
        except Exception as e:
            print("      | reading it: %s" % e)
    if got:
        pw, ph, rgb, good = got
        c.add("it is the window's size, smaller than the screen (%dx%d)" % (pw, ph),
              good and 200 < pw < 1024 and 150 < ph < 768)
        page = count_rgb(rgb, TERMINAL)
        c.add("and it holds the window: the terminal's page as the screen shows it (%d of %d), and little else" %
              (page, on_screen), abs(page - on_screen) <= on_screen // 50 and page > pw * ph // 2)
    else:
        c.add("it is the window's size, smaller than the screen", False)
        c.add("and it holds the window", False)
    if not keep:
        try:
            os.remove(DISK)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
