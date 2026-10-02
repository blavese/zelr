"""A machine with no network card joins a wireless network and uses it.

QEMU has no wireless card to give a guest, so the radio is the simulated
access point the self test joins (kernel/wlansim.c), made the machine's
network by the "wlansim" word on its command line: everything a wireless
card's frames go through -- the station (kernel/wlan.c), CCMP, the network
card, the stack -- with the card the one thing left out. It is driven as a
person drives it, twice: through the shell (what there is, a scan, a wrong
password, the right one, the address that follows by itself, dhcp and ping
over the air, and leaving), and on a second machine through Settings'
Wireless page with the mouse and keyboard, after which the shell must find
the machine joined and answering over the air.

The self test's [wireless] checks the station frame by frame; this checks
that the rest of the machine reaches it and gets an address through it.

  python tools/wificheck.py [--keep]
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "wificheck.%d.img" % os.getpid())
DISK2 = os.path.join(ROOT, "wificheck2.%d.img" % os.getpid())

# Settings as the first window of the session, as setcheck.py places it,
# and its Wireless page as settings.c lays it out: the content starts past
# the sidebar, a section heading is 34 high, a row 28, a button 30.
WIN_X, WIN_Y = 124, 36
IN_X, IN_Y = WIN_X + 1, WIN_Y + 33
SIDEBAR_W, UI_PAD, UI_ROW = 160, 8, 26
PAGES = ["Colours", "Dock", "Windows", "Desktop", "Behaviour",
         "Screen", "Wireless", "Everything", "The file", "System", "About"]
X0 = IN_X + SIDEBAR_W + UI_PAD * 2
LOOK = (X0 + 85, IN_Y + 66 + 15)
ROW0 = (X0 + 100, IN_Y + 142 + 13)
FIELD = (X0 + 130, IN_Y + 372 + 15)
JOIN = (X0 + 270 + 50, IN_Y + 372 + 15)
# The desktop's own menu, which closes the terminal so Settings is the first
# window and lands where the numbers above say (setcheck.py does the same).
CTX_W, CTX_ITEM, CTX_PAD = 204, 30, 8
CTX_X, CTX_Y = 1024 - CTX_W - 4, 200
CTX_CLOSE = 4
DOCK_H, DOCK_GAP, DOCK_SIDE = 44, 14, 16
PANEL_Y = 768 - DOCK_H - DOCK_GAP


def sidebar(name):
    i = PAGES.index(name)
    return (IN_X + 80, IN_Y + UI_PAD + i * (UI_ROW + 2) + UI_ROW // 2)


def key_of(ch):
    return {" ": "spc", ".": "dot", "-": "minus"}.get(ch, ch)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("wireless, end to end")
    vm = Guest(DISK, memory=256, args="console wlansim", extra=["-nic", "none"])
    try:
        boot = vm.wait_boot() and vm.serial()
        c.add("a machine with no card boots with the simulated radio as its network",
              "simulated wireless, not joined" in boot)
        out = vm.fresh("wifi")
        c.add("wifi says which radio, and that nothing is joined",
              "radio    simulated wireless" in out and "not joined" in out)
        out = vm.fresh("wifi scan", timeout=60)
        c.add("a scan lists the network, its channel, and that it is WPA2",
              re.search(r"zelr-sim\s+channel\s+6\s+wpa2", out) is not None)
        out = vm.fresh("wifi join zelr-sim not the password", timeout=60)
        c.add("the wrong password is not joined, and the reason says so",
              "not joined:" in out and "password" in out)
        out = vm.fresh("wifi join zelr-sim only the test knows", timeout=60)
        c.add("the right one, spaces and all, is joined", "joined zelr-sim" in out and "not joined" not in out)
        # The address follows the join by itself (net.c), as it does a
        # cable at startup: nobody has to know to type dhcp.
        end = time.time() + 30
        out = ""
        while time.time() < end and "address  10.77.0.2" not in out:
            out = vm.fresh("net")
        c.add("an address comes over the air by itself once joined", "address  10.77.0.2" in out)
        out = vm.fresh("dhcp", timeout=60)
        c.add("and asking again by dhcp gets it again", "got 10.77.0.2" in out)
        out = vm.fresh("ping 10.77.0.1", timeout=60)
        replies = len(re.findall(r"(?m)^reply from 10\.77\.0\.1", out))
        c.add("and the access point answers every ping (%d of 4)" % replies, replies == 4)
        out = vm.fresh("net")
        c.add("net shows the radio as the card, with the address and the router",
              "card     simulated wireless" in out and "address  10.77.0.2" in out and "gateway  10.77.0.1" in out)
        out = vm.fresh("wifi")
        c.add("and wifi says it is joined", "joined   zelr-sim" in out)
        out = vm.fresh("wifi leave")
        end = time.time() + 30
        out2 = ""
        while time.time() < end and "no address yet" not in out2:
            out2 = vm.fresh("ping 10.77.0.1", timeout=60)
        got = len(re.findall(r"(?m)^reply from", out2))
        c.add("after leaving, the address is forgotten and nothing gets through (%d replies)" % got,
              "left" in out and got == 0 and "no address yet" in out2)
    finally:
        vm.stop()
        if not keep:
            try:
                os.remove(DISK)
            except OSError:
                pass

    vm = Guest(DISK2, memory=256, args="console wlansim", extra=["-nic", "none"])
    try:
        vm.wait_boot()
        desktop(vm, c)
    finally:
        vm.stop()
        if not keep:
            try:
                os.remove(DISK2)
            except OSError:
                pass
    return c.report(keep=keep)


def desktop(vm, c):
    """Settings' Wireless page, driven with the mouse and the keyboard. The
    window says each thing it shows on the console too ("settings: wireless
    ..."), which is what is waited for; each click is tried again if what it
    should have shown does not come, since a busy host can lose one."""
    def said_since(mark, text, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if text in vm.serial()[mark:]:
                return True
            time.sleep(0.2)
        return False

    def click_until(at, text, timeout=20, tries=3):
        for _ in range(tries):
            mark = len(vm.serial())
            mon.click(*at)
            if said_since(mark, text, timeout):
                return True
        return False

    def key(name, settle=0.08):
        mon.send("sendkey %s" % name, settle=settle)

    vm.type("desktop\n")
    mon = vm.monitor()
    _, _, _, _, up = mon.wait_screen("wifi-desk", lambda w, h, px: True, timeout=60)
    time.sleep(3)
    # The terminal away, then Settings from the launcher, by name.
    mon.move_to(950, 200)
    mon.send("mouse_button 2", settle=0.4)
    mon.send("mouse_button 0", settle=1.2)
    mon.click(CTX_X + 60, CTX_Y + CTX_PAD + CTX_CLOSE * CTX_ITEM + CTX_ITEM // 2)
    time.sleep(2.5)
    mon.click(DOCK_SIDE + 16 + 38, PANEL_Y + DOCK_H // 2)
    time.sleep(1.5)
    for ch in "sett":
        key(ch, settle=0.12)
    key("ret", settle=0.5)
    time.sleep(4)

    shown = click_until(sidebar("Wireless"), "settings: wireless Not joined", timeout=15)
    c.add("Settings has a Wireless page, and it says nothing is joined", shown)
    if not shown:
        return
    heard = click_until(LOOK, "settings: wireless heard zelr-sim (wpa2, channel 6)", timeout=40)
    c.add("looking for networks lists the network, how it is protected and its channel", heard)
    # Chosen, which gives the password field the keyboard; a wrong password,
    # and return.
    mark = len(vm.serial())
    mon.click(*ROW0)
    time.sleep(1)
    for ch in "not the password":
        key(key_of(ch))
    key("ret")
    refused = said_since(mark, "settings: wireless Not joined: ", 40)
    tail = vm.serial()[mark:]
    c.add("a wrong password is refused, and the window says why",
          refused and re.search(r"settings: wireless Not joined: [^\n]*password", tail) is not None)
    # The field again, emptied, the right one, and the join button.
    mon.click(*FIELD)
    time.sleep(0.5)
    for _ in range(len("not the password")):
        key("backspace", settle=0.03)
    for ch in "only the test knows":
        key(key_of(ch))
    joined = click_until(JOIN, "settings: wireless Joined zelr-sim, address 10.77.0.2", timeout=40)
    c.add("the right one joins, and the window shows the address that follows", joined)
    typed = vm.serial()[mark:]
    c.add("and the password is never put on the console", "only the test" not in typed and "not the pass" not in typed)

    # Then the machine, from the shell: joined, and answering over the air.
    mon.send("sendkey esc", settle=1.0)
    time.sleep(2.0)
    vm.wait_prompt(timeout=30)
    out = vm.fresh("wifi")
    c.add("the shell finds the machine joined", "joined   zelr-sim" in out)
    out = vm.fresh("ping 10.77.0.1", timeout=60)
    replies = len(re.findall(r"(?m)^reply from 10\.77\.0\.1", out))
    c.add("and the access point answers (%d of 4)" % replies, replies == 4)


if __name__ == "__main__":
    sys.exit(main())
