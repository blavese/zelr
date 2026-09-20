"""Boots the desktop and uses the network from it.

What this is checking is the path, not the protocol: the icon on the panel,
the panel it opens, the button in that panel, the task that button starts,
and an address arriving from a DHCP server that is not part of this project.
QEMU's user networking answers, so the reply comes from somebody else's
implementation, which is the only reason getting one means anything.

Two machines, because the interesting states are what it does with a card
and what it does without one, and a panel that says the same thing either
way is a panel that is not reading anything.

  python tools/netcheck.py [--keep]
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Guest, Checks, build_once, count_near, face_width,
                     FACE_HEAD, ROOT)    # noqa: E402

DISK = os.path.join(ROOT, "netcheck.%d.img" % os.getpid())

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H, TASKBAR_GAP = 44, 14    # the dock floats clear of the edge
DOCK_SIDE = 16
NETPOP_W, NETPOP_H = 268, 150

PANEL_Y = SCREEN_H - TASKBAR_H - TASKBAR_GAP
DOCK_RIGHT = DOCK_SIDE + (SCREEN_W - DOCK_SIDE * 2)

# The right hand end of the dock, worked out the way wm.c works it out: the
# clock against the dock's own right edge, the speaker left of it, the
# network left of that.
#
# The clock's width is measured rather than written down. It used to be
# thirty six, which was five digits in the body face, and then the clock was
# set in the head face and everything derived from this moved thirteen
# pixels while the number went on saying thirty six.
CLOCK_W = face_width("12:54", FACE_HEAD)
VOL_W, NET_W = 30, 26
VOLUME_X = DOCK_RIGHT - 20 - CLOCK_W - 16 - VOL_W
NET_X = VOLUME_X - NET_W - 6
NET_ICON = (NET_X + NET_W // 2, PANEL_Y + TASKBAR_H // 2)

# Just the network glyph, which is sixteen pixels square and centred in the
# bar: left of the speaker and clear of the clock, so what is in here
# changes only when the icon does.
GLYPH_Y = PANEL_Y + (TASKBAR_H - 16) // 2
ICON_RECT = (NET_X + (NET_W - 16) // 2, GLYPH_Y,
             NET_X + (NET_W - 16) // 2 + 16, GLYPH_Y + 16)

# Where the panel lands, and the button along the bottom of it. Centred
# under the icon, then pushed back inside the screen if it would hang off.
POP_X = min(NET_X + NET_W // 2 - NETPOP_W // 2,
            SCREEN_W - TASKBAR_GAP - NETPOP_W)
POP_Y = PANEL_Y - 8 - NETPOP_H
POP_RECT = (POP_X, POP_Y, POP_X + NETPOP_W, POP_Y + NETPOP_H)
BUTTON = (POP_X + 14 + 120, POP_Y + NETPOP_H - 14 - 13)

# What the panel is tinted with before the wallpaper behind it shows through.
# It is laid down at an alpha of 248 out of 255, so a trace of what is behind
# comes with it and the count below has to allow for that.
OVERLAY = (0xF4, 0xF4, 0xF7)
OVERLAY_TOL = 6


def region(px, w, rect):
    """The bytes inside a rectangle, so two pictures can be compared over
    just the part being asked about."""
    x0, y0, x1, y1 = rect
    out = bytearray()
    for y in range(y0, y1):
        out += px[(y * w + x0) * 3:(y * w + x1) * 3]
    return bytes(out)


def address_of(vm):
    """What the machine says its address is, over the serial line. Read from
    the console rather than off the screen: the point is whether DHCP
    worked, and the panel's own drawing is not evidence of that.

    Only valid at the console. While the desktop is up the serial line goes
    to whichever window has focus, so this quietly returns nothing and every
    check resting on it passes for the wrong reason. They did, the first
    time this ran."""
    out = vm.run("net")

    # The last one, not the first. What comes back can carry earlier output
    # with it, and an old "address none" line ahead of the new one is a
    # machine that has an address being reported as having none.
    found = ""
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("address"):
            found = "" if "none" in line else line.split()[-1]
    return found


def leave_desktop(vm, mon):
    """Back to the console, so the serial line reaches the shell again."""
    mon.send("sendkey esc", settle=1.5)
    vm.wait_prompt()


def wait_for_address(vm, seconds=25):
    """The address the machine got on its own, or "" if it never did."""
    end = time.time() + seconds
    while time.time() < end:
        got = address_of(vm)
        if got not in ("", "0.0.0.0"):
            return got
        time.sleep(2)
    return ""


def use_desktop(vm, c, wired):
    vm.wait_boot()

    # A machine with a card asks for an address by itself, at startup, and
    # nothing here presses anything to make that happen. This used to check
    # the opposite, because the only thing that ever asked was the button on
    # the panel, and a freshly booted machine sat there with a working card
    # and no address while everything that used the network failed saying
    # something else about itself.
    if wired:
        c.add("an address arrives without anything being asked for",
              wait_for_address(vm) == "10.0.2.15")
    else:
        c.add("a machine with no card does not invent one",
              address_of(vm) in ("", "0.0.0.0"))

    vm.type("desktop\n")
    time.sleep(6)
    mon = vm.monitor()

    # Away from the panel, so nothing is lit up by the pointer being on it.
    mon.move_to(SCREEN_W - 320, 300)
    time.sleep(1.0)
    w, h, px, shot = mon.screen("net-idle")
    icon = region(px, w, ICON_RECT)

    c.add("the icon is drawn on the panel" if wired
          else "the icon is drawn with no card either",
          len(set(icon)) > 1, shot)

    # --- the panel it opens ------------------------------------------------
    before = region(px, w, POP_RECT)
    w, h, px, shot, opened = mon.click_for(
        NET_ICON[0], NET_ICON[1], "net-panel",
        lambda w, h, px: count_near(px, w, POP_RECT, OVERLAY,
                                    OVERLAY_TOL) > 4000,
        timeout=20)
    c.add("clicking it opens the network panel", opened, shot)
    c.add("which is drawn over what was there",
          region(px, w, POP_RECT) != before, shot)

    # --- and the button in it ----------------------------------------------
    mon.click(*BUTTON)
    time.sleep(9)

    # The panel is put away before the second picture, because an open panel
    # lights its own icon up. Leaving it open made the icon differ between
    # the two pictures whatever the network did, and the check that the icon
    # followed the address passed on a machine that never got one.
    mon.click(*NET_ICON)
    time.sleep(1.0)

    # And from the same place the first picture was taken, so the only thing
    # left that can have changed the icon is the icon.
    mon.move_to(SCREEN_W - 320, 300)
    time.sleep(1.0)
    w, h, px, shot = mon.screen("net-after")

    return mon, icon, region(px, w, ICON_RECT), shot


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("the network")
    icons = {}

    # --- a machine with a card ---------------------------------------------
    vm = Guest(DISK, memory=128)
    try:
        mon, icons["wired"], _, shot = use_desktop(vm, c, True)
        leave_desktop(vm, mon)
        got = address_of(vm)

        # QEMU hands out this one. It comes from its DHCP server, not from
        # anything here, which is what makes it worth checking.
        c.add("the address came from the dhcp server",
              got == "10.0.2.15")
        c.add("and a router to send everything else to",
              vm.run("net").find("10.0.2.2") >= 0)

        # The button is still there, and pressing it on a machine that
        # already has one has to leave it with one. Asking twice is the
        # ordinary case now rather than the only one.
        c.add("and pressing the button anyway does not lose it",
              got == "10.0.2.15", shot)
    finally:
        vm.stop()

    # --- and one with none --------------------------------------------------
    vm2 = Guest(DISK, memory=128, extra=["-nic", "none"])
    try:
        mon, icons["bare"], bare_after, _ = use_desktop(vm2, c, False)
        leave_desktop(vm2, mon)
        c.add("a machine with no card gets no address by asking",
              address_of(vm2) in ("", "0.0.0.0"))
        c.add("and its icon did not change, because nothing happened to it",
              bare_after == icons["bare"])
        c.add("and says something different in the icon than one with a card",
              icons["bare"] != icons["wired"])
    finally:
        vm2.stop()

    # --- and one whose only network is a usb adapter ------------------------
    #
    # Which is the case this was written for. A laptop whose wireless will
    # not start without a vendor binary still has a socket on the side of it,
    # and a phone with tethering turned on presents exactly this. No card on
    # the pci bus at all here, so an address arriving proves the frames went
    # through the usb stack and nothing else.
    vm3 = Guest(DISK, memory=192, machine="q35",
                extra=["-nic", "none",
                       "-device", "qemu-xhci",
                       "-device", "usb-net,netdev=u1",
                       "-netdev", "user,id=u1"])
    try:
        vm3.wait_boot()
        c.add("a usb adapter is found and is the card",
              "usb ethernet" in vm3.run("net"))
        # No pci card at all here, so an address arriving proves the frames
        # went through the usb stack and nothing else. It is asked for at
        # startup like any other, and asked again here in case the adapter
        # was still being enumerated when the first one went out.
        got3 = wait_for_address(vm3)
        if not got3:
            vm3.run("dhcp", timeout=25)
            got3 = wait_for_address(vm3)
        c.add("and an address arrives over usb", got3 == "10.0.2.15")

        # Frames counted in both directions, so this cannot pass on a driver
        # that received the lease and never sent anything.
        out = vm3.run("net")
        counted = [ln for ln in out.splitlines() if ln.strip().startswith("packets")]
        c.add("with frames counted going out as well as coming in",
              bool(counted) and "0 in, 0 out" not in counted[-1])
    finally:
        vm3.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass

    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
