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
from harness import Guest, Checks, build_once, count_in, ROOT    # noqa: E402

DISK = os.path.join(ROOT, "netcheck.%d.img" % os.getpid())

SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H, TASKBAR_GAP = 34, 10
NETPOP_W, NETPOP_H = 268, 150

# The right hand end of the panel, worked out the way wm.c works it out: the
# clock against the edge, the speaker left of it, the network left of that.
# The clock is always five characters, so this does not move.
NET_ICON = (SCREEN_W - TASKBAR_GAP - 16 - 48 - 14 - 30 - 6 - 13,
            SCREEN_H - TASKBAR_H - TASKBAR_GAP + 17)

# Just the network icon: left of the speaker, right of the app icons, and
# clear of the clock, so what is in here changes only when the icon does.
ICON_RECT = (880, 728, 916, 752)

# Where the panel lands, and the button along the bottom of it.
POP_X = SCREEN_W - TASKBAR_GAP - NETPOP_W
POP_Y = SCREEN_H - TASKBAR_H - TASKBAR_GAP - 8 - NETPOP_H
POP_RECT = (POP_X, POP_Y, POP_X + NETPOP_W, POP_Y + NETPOP_H)
BUTTON = (POP_X + 14 + 120, POP_Y + NETPOP_H - 14 - 13)

OVERLAY = (0x3F, 0x46, 0x4D)      # the floating layer the panel is drawn on


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


def use_desktop(vm, c, wired):
    vm.wait_boot()

    # Asked at the console, which is the only place the answer can be read.
    c.add("there is no address before anything is asked for",
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
        lambda w, h, px: count_in(px, w, POP_RECT, OVERLAY) > 4000,
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
        mon, icons["wired"], after, shot = use_desktop(vm, c, True)
        leave_desktop(vm, mon)
        got = address_of(vm)

        # QEMU hands out this one. It comes from its DHCP server, not from
        # anything here, which is what makes it worth checking.
        c.add("asking gets an address from the dhcp server",
              got == "10.0.2.15")
        c.add("and a router to send everything else to",
              vm.run("net").find("10.0.2.2") >= 0)

        c.add("and the icon changed once there was an address",
              after != icons["wired"], shot)
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
        c.add("it has an address of its own before any of ours",
              address_of(vm3) in ("", "0.0.0.0"))

        vm3.run("dhcp", timeout=25)
        c.add("and an address arrives over usb",
              address_of(vm3) == "10.0.2.15")

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
