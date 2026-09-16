"""Boots with a USB keyboard and mouse and checks they work.

Every other test in this project types over the serial line or drives the PS/2
keyboard, and neither says anything about USB. This one gives the machine an
xHCI controller with a keyboard and a mouse on it, and then uses them.

What makes the result mean something is that QEMU delivers a key press to one
keyboard, not to all of them. With a USB keyboard attached it is the one that
gets the keys, and the PS/2 keyboard that the machine also has receives
nothing at all. So a key typed here that reaches the shell can only have
arrived through the controller, the event ring, the interrupt endpoint and
the boot protocol decoder. If any of that is broken, nothing is typed and
nothing happens, which is exactly the failure a laptop would have.

  python tools/usbcheck.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

DISK = os.path.join(ROOT, "usbcheck.%d.img" % os.getpid())

USB = ["-device", "qemu-xhci,id=xhci",
       "-device", "usb-kbd,bus=xhci.0",
       "-device", "usb-mouse,bus=xhci.0"]

# The same two devices, one hub further away. On qemu-xhci the first four
# ports are the USB 2 ones and the last four are USB 3 only, and a 2.0 hub
# has to go on a 2.0 port, so the hub is on port 1 and 1.1 and 1.2 are its
# own ports.
#
# Nothing else in this project covers this, and it is the shape a laptop
# actually has: the built-in keyboard is very often behind a hub inside the
# chipset rather than on a port of the controller itself. A driver that walks
# only root ports finds nothing on a machine like this and is not obviously
# broken until somebody tries it on hardware.
HUB = ["-device", "qemu-xhci,id=xhci",
       "-device", "usb-hub,bus=xhci.0,port=1",
       "-device", "usb-kbd,bus=xhci.0,port=1.1",
       "-device", "usb-mouse,bus=xhci.0,port=1.2"]

# A machine that starts with a mouse and no keyboard on USB at all, so that
# a keyboard turning up later is unambiguous.
HOTPLUG = ["-device", "qemu-xhci,id=xhci",
           "-device", "usb-mouse,bus=xhci.0,id=mouse"]

# A stick is a drive plus a usb-storage device in front of it, the same
# shape as a real one: the controller sees bulk endpoints and a SCSI target
# behind them, and has no idea the medium is a file on the host.
STICK_MB = 8
PATTERN = bytes((0xDE, 0xAD, 0xBE, 0xEF)) * 4
PATTERN_LBA = 100
WRITE_LBA = 200
WRITE_BYTE = 0xAB


def make_stick(path):
    """An empty disk with something recognisable at a known sector."""
    with open(path, "wb") as f:
        f.truncate(STICK_MB * 1024 * 1024)
        f.seek(PATTERN_LBA * 512)
        f.write(PATTERN + bytes(512 - len(PATTERN)))
    return ["-drive", "if=none,id=stick,format=raw,file=%s" % path,
            "-device", "usb-storage,bus=xhci.0,drive=stick"]


NAMED = {" ": "spc", "\n": "ret", "/": "slash", ".": "dot", "-": "minus"}


def keys(mon, text, settle=0.08):
    for ch in text:
        mon.send("sendkey %s" % NAMED.get(ch, ch), settle=settle)


def reports(text):
    """The report count out of /sys/devices, or -1.

    The number wanted is the one in front of the word report, not the first
    number on the line: the line also carries how many ports and slots the
    controller has, and both of those sit before it and never change.

    The last one on the console, not the first: what comes back is everything
    the machine has ever said, so an earlier reading of the same file is
    still in there and is the one that would be found."""
    found = -1
    for line in text.splitlines():
        words = line.strip().replace(",", " ").split()
        if not words or words[0] != "usb":
            continue
        for i, word in enumerate(words):
            if word.startswith("report") and i and words[i - 1].isdigit():
                found = int(words[i - 1])
    return found


def main():
    build_once()
    vm = Guest(DISK, memory=128, machine="q35", extra=USB)
    c = Checks("usb test")

    try:
        vm.wait_boot()
        boot = vm.serial()

        # --- did it find anything ------------------------------------------
        c.add("the controller came up", "xhci" in boot)
        c.add("a keyboard and a mouse were enumerated",
              "1 keyboard(s), 1 mouse" in boot)

        out = vm.run("cat /sys/devices")
        before = reports(out)
        c.add("the device list says there is usb", before >= 0)
        c.add("and that the keyboard is one of them",
              "ps/2 and usb" in out)

        # --- type on it ------------------------------------------------------
        #
        # Through the monitor, which with a USB keyboard attached is the
        # keyboard these go to. The command is one whose output cannot be
        # confused with anything already on the console.
        mon = vm.monitor()
        marks = vm.prompts()
        keys(mon, "uname\n")
        typed = vm.wait_prompt(marks + 1, timeout=30)
        c.add("a key pressed on the usb keyboard reached the shell", typed)

        # What uname prints and nothing else does. Counting the word zelr
        # instead looks like it works and is not a check at all: it is in
        # every shell prompt, so the count climbs whatever was typed, and it
        # went on passing with the keyboard deliberately broken.
        after_text = vm.serial()
        c.add("and the command it typed actually ran",
              "x86_64" in after_text)

        out = vm.run("cat /sys/devices")
        after = reports(out)
        c.add("the controller delivered reports while that happened",
              after > before >= 0)

        # --- and the mouse ----------------------------------------------------
        #
        # The desktop is what draws a pointer, so it has to be running before
        # there is anything to look at. It is started by typing at the USB
        # keyboard as well, which is the second time that path is used.
        keys(mon, "desktop\n")
        found = None
        for _ in range(60):
            found = mon.pointer()
            if found:
                break
            time.sleep(0.5)
        c.add("the desktop starts, typed on the usb keyboard", found is not None)

        if found:
            mon.send("mouse_move 60 40", settle=0.4)
            mon.send("mouse_move 60 40", settle=0.4)
            moved = None
            for _ in range(20):
                moved = mon.pointer()
                if moved and moved != found:
                    break
                time.sleep(0.3)
            c.add("moving the usb mouse moves the pointer",
                  moved is not None and moved != found)
        else:
            c.add("moving the usb mouse moves the pointer", False)
    finally:
        vm.stop()

    check_behind_a_hub(c)
    check_hot_plug(c)
    check_a_stick(c)
    return c.report()


def check_behind_a_hub(c):
    """The same machine again, with everything one hub further away."""
    vm = Guest(os.path.join(ROOT, "usbhub.%d.img" % os.getpid()),
               memory=128, machine="q35", extra=HUB)
    try:
        vm.wait_boot()
        boot = vm.serial()
        c.add("a hub is found and walked", "1 hub(s)" in boot)
        c.add("and the keyboard and mouse behind it",
              "1 keyboard(s), 1 mouse" in boot)

        # Typing is the part that proves the route string and the transaction
        # translator were right, rather than only that a descriptor was read.
        mon = vm.monitor()
        marks = vm.prompts()
        keys(mon, "uname\n")
        typed = vm.wait_prompt(marks + 1, timeout=30)
        c.add("a key on a keyboard behind a hub reaches the shell", typed)
        c.add("and the command it typed ran", "x86_64" in vm.serial())
    finally:
        vm.stop()

def devices(vm, text, timeout=25):
    """Waits for the device list to say something.

    The task that watches for a port changing polls, so none of this is
    instant, and asking once and calling it a failure would be a test of the
    timing rather than of the driver."""
    end = time.time() + timeout
    while True:
        if text in vm.run("cat /sys/devices"):
            return True
        if time.time() > end:
            return False
        time.sleep(1)


def check_hot_plug(c):
    """Something plugged in while the machine is already running.

    Enumeration at boot happens in the quietest conditions the machine ever
    has: interrupts are still off, nothing else is running, and the whole
    thing is one straight line of code. This is the same work done from a
    task, with the scheduler going and interrupts on, which is the only way
    it ever happens on a machine somebody is using. It is also the one path
    that cannot be reached by starting QEMU with the device already there,
    so nothing else in this file covers it.
    """
    vm = Guest(os.path.join(ROOT, "usbhot.%d.img" % os.getpid()),
               memory=128, machine="q35", extra=HOTPLUG)
    try:
        vm.wait_boot()
        c.add("a machine can start with no usb keyboard",
              "0 keyboard(s)" in vm.run("cat /sys/devices"))

        mon = vm.monitor()
        mon.send("device_add usb-kbd,bus=xhci.0,id=latecomer")
        plugged = devices(vm, "1 keyboard(s)")
        c.add("a keyboard plugged in later is noticed", plugged)

        # Noticing it is not the same as being able to use it: the slot has
        # to have been addressed, configured and left listening.
        marks = vm.prompts()
        keys(mon, "uname" + chr(10))
        typed = vm.wait_prompt(marks + 1, timeout=30)
        c.add("and it can be typed on", typed and "x86_64" in vm.serial())

        # Only says anything if there was one there to pull out. Without
        # that, a machine which never noticed the keyboard at all still
        # reads as zero keyboards and this passes for the wrong reason.
        mon.send("device_del latecomer")
        c.add("and pulling it out is noticed too",
              plugged and devices(vm, "0 keyboard(s)"))
    finally:
        vm.stop()




def fresh(vm, line):
    """What a command printed, rather than everything the machine has said.

    Guest.run hands back the whole console, boot log included, so a check
    for a string that also appears at boot passes whether the command
    printed it or not. Found by breaking one: the size line was reworded,
    and the check for it kept passing on the boot log's copy of the same
    words.
    """
    mark = len(vm.serial())
    vm.run(line)
    return vm.serial()[mark:]


def check_a_stick(c):
    """A USB stick, enumerated and then actually used.

    Enumerating one proves almost nothing. A device will report a capacity it
    cannot move a byte of, and the three bulk transfers that make up a SCSI
    command each fail in their own way. So this reads a sector the host put a
    pattern in, writes a different sector, and then checks from the host that
    the write reached the file. A driver that only looks like it works cannot
    pass the last of those.
    """
    img = os.path.join(ROOT, "stick.%d.img" % os.getpid())
    extra = ["-device", "qemu-xhci,id=xhci"] + make_stick(img)

    vm = Guest(os.path.join(ROOT, "usbstick.%d.img" % os.getpid()),
               memory=256, machine="q35", extra=extra)
    try:
        vm.wait_boot()
        c.add("a usb stick is found", "1 disk(s)" in vm.serial())

        out = fresh(vm, "stick")
        c.add("and says how big it is", "sectors of 512 bytes" in out)

        # 8 MiB is 16384 sectors. Anything wildly different means the
        # capacity came back as something other than what was attached.
        c.add("and the size is the size of the disk attached",
              "16384 sectors" in out)

        out = fresh(vm, "stick read %d" % PATTERN_LBA)
        c.add("a sector written by the host reads back",
              "de ad be ef" in out.lower())

        out = fresh(vm, "stick write %d %d" % (WRITE_LBA, WRITE_BYTE))
        c.add("a sector can be written", "wrote sector" in out)

        out = fresh(vm, "stick read %d" % WRITE_LBA)
        want = ("%x " % WRITE_BYTE) * 3
        c.add("and reads back as what was written", want in out.lower() + " ")
    finally:
        vm.stop()

    # The strongest of these, and the only one the guest cannot fake: the
    # bytes are on the host's disk now.
    landed = False
    try:
        with open(img, "rb") as f:
            f.seek(WRITE_LBA * 512)
            block = f.read(512)
        landed = len(block) == 512 and all(b == WRITE_BYTE for b in block)
    except OSError:
        landed = False
    c.add("and the bytes are on the host's disk afterwards", landed)

    try:
        os.remove(img)
    except OSError:
        pass


if __name__ == "__main__":
    sys.exit(main())
