"""A machine with no network card joins a wireless network and uses it.

QEMU has no wireless card to give a guest, so the radio is the simulated
access point the self test joins (kernel/wlansim.c), made the machine's
network by the "wlansim" word on its command line: everything a wireless
card's frames go through -- the station (kernel/wlan.c), CCMP, the network
device, the stack -- with the card the one thing left out. It is driven as a
person drives it, through the shell: what there is, a scan, a wrong password,
the right one, then dhcp and ping over the air, and leaving.

The self test's [wireless] checks the station frame by frame; this checks
that the rest of the machine reaches it and gets an address through it.

  python tools/wificheck.py [--keep]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "wificheck.%d.img" % os.getpid())


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
        out = vm.fresh("dhcp", timeout=60)
        c.add("an address comes by dhcp over the air", "got 10.77.0.2" in out)
        out = vm.fresh("ping 10.77.0.1", timeout=60)
        replies = len(re.findall(r"(?m)^reply from 10\.77\.0\.1", out))
        c.add("and the access point answers every ping (%d of 4)" % replies, replies == 4)
        out = vm.fresh("net")
        c.add("net shows the radio as the card, with the address and the router",
              "card     simulated wireless" in out and "address  10.77.0.2" in out and "gateway  10.77.0.1" in out)
        out = vm.fresh("wifi")
        c.add("and wifi says it is joined", "joined   zelr-sim" in out)
        out = vm.fresh("wifi leave")
        out2 = vm.fresh("ping 10.77.0.1", timeout=60)
        # "no reply from" holds the words "reply from": a reply is a line that starts with them.
        got = len(re.findall(r"(?m)^reply from", out2))
        c.add("after leaving, nothing gets through (%d replies)" % got,
              "left" in out and got == 0 and out2.count("no reply from") == 4)
    finally:
        vm.stop()
        if not keep:
            try:
                os.remove(DISK)
            except OSError:
                pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
