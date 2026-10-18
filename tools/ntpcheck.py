"""The clock set from a time server, on a machine whose own clock is wrong.

QEMU's clock is put back to the first of June 2020, which is what a flat
battery does to a real one, and a time server runs on this side (SNTP, a
few lines of the standard library) at the address the guest reaches the
host by. The disk's /zelr.cfg names that server. Once the machine has an
address it asks, and from then on its time is this machine's to the
second, though its clock still says 2020: /sys/time, the shell's date and
a file written are all told the real time.

The server misbehaves on two more ports, one way each: an answer to some
other question (which net.c never takes, so there is no answer), and a
refusal (stratum nought). Asked there with `timesync`, the machine says why
and keeps the time it had.

  python tools/ntpcheck.py [--keep]
"""
import datetime
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT                       # noqa: E402
from readfat import Fat16                                                 # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "ntpcheck.%d.img" % os.getpid())
CFG_SRC = os.path.join(ROOT, "ntpcheck.%d.cfg" % os.getpid())

NTP_UNIX_GAP = 2208988800
WRONG_CLOCK = "2020-06-01T00:00:00"


def ntp_stamp(t):
    secs = int(t) + NTP_UNIX_GAP
    frac = int((t - int(t)) * (1 << 32))
    return struct.pack(">II", secs & 0xFFFFFFFF, frac)


def serve(sock, kind, stop):
    """Answers SNTP questions: kind "good", "stranger" (the originate stamp
    is not the question's), or "refuse" (stratum nought)."""
    sock.settimeout(0.2)
    while not stop.is_set():
        try:
            q, addr = sock.recvfrom(512)
        except socket.timeout:
            continue
        except OSError:
            return
        if len(q) < 48:
            continue
        now = time.time()
        a = bytearray(48)
        a[0] = 0x24                                   # no warning, version 4, a server
        a[1] = 0 if kind == "refuse" else 2
        a[24:32] = q[40:48] if kind != "stranger" else bytes(8)
        a[32:40] = ntp_stamp(now)
        a[40:48] = ntp_stamp(now)
        sock.sendto(bytes(a), addr)


def stamp(text, key):
    m = re.search(r"^%s\s+(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)" % key, text, re.M)
    return datetime.datetime(*map(int, m.groups())) if m else None


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("network time")

    stop = threading.Event()
    socks = []
    ports = {}
    for kind in ("good", "stranger", "refuse"):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        ports[kind] = s.getsockname()[1]
        socks.append(s)
        threading.Thread(target=serve, args=(s, kind, stop), daemon=True).start()
    server = "10.0.2.2:%d" % ports["good"]

    with open(CFG_SRC, "w") as f:
        f.write("timeserver %s\nzone Asia/Kolkata\n" % server)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768",
                        CFG_SRC + ":zelr.cfg"], cwd=ROOT, capture_output=True, text=True)
    c.add("a volume whose settings name a time server here", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True, extra=["-rtc", "base=" + WRONG_CLOCK])
    try:
        vm.wait_boot()

        # Asked once the address is bound, by itself.
        st = ""
        end = time.time() + 90
        while time.time() < end:
            st = vm.fresh("cat /sys/time")
            if "network   set from" in st or "network   not set" in st:
                break
            time.sleep(1)
        said = re.search(r"^network\s+(.*)$", st, re.M)
        c.add("the machine asks the time server once it has an address (%s)"
              % (said.group(1).strip() if said else None),
              "network   set from %s, the clock was" % server in st and "s slow" in st)

        host = datetime.datetime.utcnow()
        now = stamp(st, "now")
        c.add("and its time is this machine's to the second, its clock at 2020 notwithstanding (%s, here %s)"
              % (now, host.strftime("%Y-%m-%d %H:%M:%S")),
              now is not None and abs((now - host).total_seconds()) <= 3)
        local = stamp(st, "local")
        c.add("told in its zone too",
              local is not None and now is not None and (local - now).total_seconds() == 330 * 60)

        out = vm.fresh("date")
        want = (host + datetime.timedelta(minutes=330))
        c.add("the shell's date says the real day", "%d %s %d" % (want.day, want.strftime("%B"), want.year) in out)

        before = datetime.datetime.utcnow()
        vm.fresh("write /ntp.txt now")
        try:
            written = Fat16(IMAGE).times("/ntp.txt")[0]
        except Exception:
            written = None
        there = before + datetime.timedelta(minutes=330)
        c.add("and a file written is stamped with it (%s)" % written,
              written is not None and abs((written - there).total_seconds()) <= 5)

        out = vm.fresh("timesync 10.0.2.2:%d" % ports["stranger"], timeout=40)
        c.add("an answer to some other question is not taken", "not set: no answer from" in out)
        out = vm.fresh("timesync 10.0.2.2:%d" % ports["refuse"], timeout=40)
        c.add("a refusal is told as one", "not set: the server refused" in out)
        st = vm.fresh("cat /sys/time")
        now = stamp(st, "now")
        host = datetime.datetime.utcnow()
        c.add("and neither moves the clock",
              now is not None and abs((now - host).total_seconds()) <= 3)
        out = vm.fresh("timesync", timeout=40)
        c.add("asked again, the clock is found right", "set from %s, the clock was right" % server in out)
    finally:
        vm.stop()
        stop.set()
        for s in socks:
            s.close()

    for junk in (CFG_SRC,) + (() if keep else (IMAGE,)):
        try:
            os.remove(junk)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
