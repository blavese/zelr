"""A USB stick with files on it, mounted and used.

Reading a stick a sector at a time proved the driver worked and proved
nothing about whether it was any use: the block layer held exactly one disk,
picked at boot, so the filesystem had no way to ask for any other one. The
stick was a device you could dump hex out of.

This puts a real FAT volume on a stick, plugs it in, and then does the thing
a person would actually do with it: list it, read a file off it, copy a file
onto it, and copy one between it and the disk the machine booted from. The
last check reads the image back on this side with tools/readfat.py, which
parses FAT from the specification rather than asking the kernel, so a file
that is there is really there.

  python tools/mountcheck.py
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
STICK = os.path.join(ROOT, "mountcheck.%d.img" % os.getpid())
DISK = os.path.join(ROOT, "mountdisk.%d.img" % os.getpid())
SEED = os.path.join(ROOT, "mountseed.%d.txt" % os.getpid())
ESP_STICK = os.path.join(ROOT, "mountesp.%d.img" % os.getpid())
ESP_DISK = os.path.join(ROOT, "mountespdisk.%d.img" % os.getpid())
ESP_VOL = os.path.join(ROOT, "mountespvol.%d.img" % os.getpid())

ON_STICK = "a file that was already on the stick"
FROM_ZELR = "written from inside the machine"

TRACE = os.path.join(ROOT, "mounttrace.%d.log" % os.getpid())

USB = ["-device", "qemu-xhci,id=xhci",
       "-drive", "if=none,id=stick,format=raw,file=" + STICK.replace("\\", "/"),
       "-device", "usb-storage,bus=xhci.0,drive=stick"]

# What each disk was actually told, from QEMU's side of the wire. A flush
# cannot be seen from inside the machine -- the call returns true either way,
# and it returned true for years while doing nothing -- and QEMU's disks write
# through, so no amount of pulling the plug here would show one missing. The
# trace is the one witness that is not the kernel's own word: every ATA
# command the AHCI disk executes, and every SCSI command the stick parses.
WATCH = ["-trace", "enable=ide_bus_exec_cmd", "-trace", "enable=scsi_req_parsed",
         "-D", TRACE.replace("\\", "/")]

ATA_FLUSH_EXT = "cmd 0xea"          # FLUSH CACHE EXT, as QEMU prints it
SCSI_SYNC_CACHE = "command 53"      # SYNCHRONIZE CACHE(10) is 0x35


def trace_now():
    try:
        with open(TRACE, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def make_stick():
    """A FAT volume with one file on it, built by our own tool."""
    with open(SEED, "w", encoding="ascii", newline="") as f:
        f.write(ON_STICK)

    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"),
                        STICK, "8192", SEED + ":HELLO.TXT"],
                       capture_output=True, text=True)
    return r.returncode == 0


def make_esp_stick():
    """A stick laid out as one that starts a computer: a partition table whose
    only entry is an EFI System Partition (type 0xEF) holding a FAT volume."""
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"),
                        ESP_VOL, "4096", SEED + ":BOOT.TXT"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return False
    with open(ESP_VOL, "rb") as f:
        vol = f.read()
    start = 2048
    count = len(vol) // 512
    mbr = bytearray(512)
    e = 446
    mbr[e + 4] = 0xEF
    mbr[e + 8:e + 12] = start.to_bytes(4, "little")
    mbr[e + 12:e + 16] = count.to_bytes(4, "little")
    mbr[510], mbr[511] = 0x55, 0xAA
    with open(ESP_STICK, "wb") as f:
        f.write(bytes(mbr))
        f.write(bytes(512 * (start - 1)))
        f.write(vol)
    return True


def on_stick_now():
    """What the stick holds, read on this side rather than asked for."""
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "readfat.py"), STICK],
                       capture_output=True, text=True)
    return r.stdout


def file_on_stick(name):
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "readfat.py"),
                        STICK, name], capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else ""


def main():
    build_once()
    c = Checks("mount test")

    if not make_stick():
        c.add("a fat volume can be built for the stick", False)
        return c.report()

    vm = Guest(DISK, memory=256, machine="q35", extra=USB + WATCH)
    try:
        vm.wait_boot()
        boot = vm.serial()
        c.add("the stick is found and mounted", "usb volume mounted" in boot)

        # fresh rather than run throughout: run hands back the whole
        # console, and the command being echoed contains the very text these
        # are looking for, so they would pass with nothing mounted at all.
        # Two of them did, until a deliberate break showed it.
        out = vm.fresh("ls /usb")
        c.add("its files are listed", "HELLO.TXT" in out.upper())

        out = vm.fresh("cat /usb/HELLO.TXT")
        c.add("and one of them can be read", ON_STICK in out)

        # Onto the stick, which is the direction that has to reach the device.
        #
        # Checked by listing rather than by reading it back, because reading
        # it back proves nothing: with nothing mounted, /usb falls through to
        # the in-memory filesystem and the file is written and read there
        # quite happily. Seeing it next to the file that was already on the
        # volume is what cannot happen unless the volume is really there.
        before = trace_now().count(SCSI_SYNC_CACHE)
        vm.run("write /usb/MADE.TXT %s" % FROM_ZELR.replace(" ", "_"))
        out = vm.fresh("ls /usb").upper()
        c.add("a file written to it joins the one already there",
              "MADE.TXT" in out and "HELLO.TXT" in out)

        # The writes that make a save crash-safe are ordered by flushes, and
        # those flushes went to the disk the machine booted from whichever
        # disk the file was on, and to the stick went nothing at all.
        c.add("saving a file on the stick tells the stick to write it down",
              trace_now().count(SCSI_SYNC_CACHE) > before)

        # And between the two volumes, which is the whole point of there
        # being more than one.
        vm.run("cp /usb/HELLO.TXT /home/COPIED.TXT")
        out = vm.fresh("cat /home/COPIED.TXT")
        c.add("a file copies from the stick to the disk", ON_STICK in out)

        out = vm.fresh("ls /")
        c.add("the root shows usb alongside bin and sys", "usb/" in out)

        # And the boot disk, which on this machine is AHCI: its flush was a
        # function that returned true and told the drive nothing.
        before = trace_now().count(ATA_FLUSH_EXT)
        vm.run("write /home/FLUSHED.TXT on_the_boot_disk")
        c.add("saving a file on the boot disk sends the drive a flush",
              trace_now().count(ATA_FLUSH_EXT) > before)

        # format means the disk the machine booted from. It used to mean
        # whichever volume the last path had touched, so after the look at
        # /usb above it would have written a fresh volume -- sized for the
        # boot disk -- over the stick.
        vm.run("ls /usb")
        out = vm.fresh("format", timeout=60)
        c.add("format after a look at the stick formats the boot disk",
              "disk formatted" in out)
        out = vm.fresh("ls /usb").upper()
        c.add("and the stick still holds its files afterwards",
              "HELLO.TXT" in out and "MADE.TXT" in out)
        out = vm.fresh("cat /home/COPIED.TXT")
        c.add("while the boot disk really was emptied", ON_STICK not in out)
    finally:
        vm.stop()

    # The strongest of them: the bytes are in the image, and something that
    # is not this kernel says so.
    listing = on_stick_now()
    c.add("the written file is really on the stick", "MADE.TXT" in listing.upper())
    c.add("and its contents are what was written",
          "written_from_inside" in file_on_stick("MADE.TXT"))

    c.add("and the stick's own volume is still intact, read from outside",
          "HELLO.TXT" in listing.upper())

    # --- a stick that starts a computer ----------------------------------------
    #
    # Its FAT volume is an EFI System Partition, which is never to be touched:
    # that held for the disk the machine started from, and a stick's was
    # mounted at /usb and written like any other.
    esp_ok = make_esp_stick()
    c.add("a stick whose only volume is its efi partition can be built", esp_ok)
    if esp_ok:
        esp_usb = ["-device", "qemu-xhci,id=xhci",
                   "-drive", "if=none,id=stick,format=raw,file=" + ESP_STICK.replace("\\", "/"),
                   "-device", "usb-storage,bus=xhci.0,drive=stick"]
        vm = Guest(ESP_DISK, memory=256, machine="q35", extra=esp_usb)
        try:
            vm.wait_boot()
            seen = vm.wait_serial("usb disk on slot", timeout=60)
            c.add("the stick is found", seen)
            vm.wait_serial("usb disk has no filesystem", timeout=20)
            boot = vm.serial()
            c.add("but its efi partition is not mounted",
                  "usb disk has no filesystem" in boot and "usb volume mounted" not in boot)
            out = vm.fresh("ls /")
            c.add("and there is no /usb to write to", "usb/" not in out)
        finally:
            vm.stop()

    for junk in (STICK, DISK, SEED, TRACE, ESP_STICK, ESP_DISK, ESP_VOL):
        try:
            os.remove(junk)
        except OSError:
            pass
    return c.report()


if __name__ == "__main__":
    sys.exit(main())
