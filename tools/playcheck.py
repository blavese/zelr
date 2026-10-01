"""Plays a stream, listens to what came out and looks at what was shown.

The player (userland/play.c) is the whole road a Twitch channel takes: a
playlist and its segments fetched over HTTP (hls.h), the transport stream
taken apart (ts.h), the AAC decoded (aac.h) and handed to the card only as
fast as it has room for it (sound_info's room), and the H.264 decoded
(h264.h) and each frame shown when the sound reaches its time. aactest and
h264test check the decoders against Windows' answers; this checks the rest
by ear and by eye, the way soundcheck checks a note. A server on the host
plays the part of Twitch's, with a finished playlist of three segments of
two seconds each, put together here as Twitch puts its own: the second of
tone in userland/aacdata.h that Windows encoded (440 Hz on the left, 660 Hz
on the right), twice, and the two seconds of flat colours in
userland/h264data.h (red beside blue, then green beside yellow), each
picture with its time in display order. QEMU writes down what the card
played and the screen is looked at while it plays.

It is started the way a person starts it: the browser, on the desktop, shows
a page from the same server that is one big `play:` link, as a live
channel's "watch" is, and the link is clicked. So the browser handing the
address to the player is checked too, and the sound has to keep up with a
desktop, a browser and the picture being decoded beside it.

What going wrong would look or sound like:
  nothing decoded or written        silence, or no picture
  the rate converted wrongly         the wrong pitch
  the card left to run dry           gaps between segments
  the end of the list mishandled     less than six seconds, or no end said
  the colours or the frames wrong    the four colours not seen, in pairs

  python tools/playcheck.py
"""
import http.server
import os
import re
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402
from soundcheck import close_to                          # noqa: E402
from browsercheck import PAGE, PARK                      # noqa: E402

DISK = os.path.join(ROOT, "playcheck.%d.img" % os.getpid())
WAV = os.path.join(ROOT, "playcheck.%d.wav" % os.getpid())

SEGMENTS = 3
FRAME = 9000          # a picture's time, at 90 kHz: the colours are ten a second
AUDIO = ["-audiodev", "wav,id=a0,path=" + WAV.replace("\\", "/"),
         "-device", "intel-hda",
         "-device", "hda-output,audiodev=a0"]

ASKED = {}


def header_array(name, header):
    text = open(os.path.join(ROOT, "userland", header), encoding="ascii").read()
    m = re.search(name + r"\[(\d+)\] = \{(.*?)\};", text, re.S)
    data = bytes(int(v) for v in re.findall(r"\d+", m.group(2)))
    if len(data) != int(m.group(1)):
        sys.exit("%s's %s is not the length it says" % (header, name))
    return data


# --- taking apart and putting together ------------------------------------------------------

def ts_payloads(ts):
    """Each PES packet's payload in a transport stream, by stream type."""
    pmt = None
    pids = {}
    pes = {}
    order = []
    for at in range(0, len(ts) - 187, 188):
        p = ts[at:at + 188]
        if p[0] != 0x47:
            continue
        pid = ((p[1] & 31) << 8) | p[2]
        start = p[1] & 0x40
        i = 4
        if (p[3] >> 4) & 2:
            i += 1 + p[4]
        if not (p[3] >> 4) & 1 or i >= 188:
            continue
        pay = p[i:]
        if pid == 0 and start:
            sec = pay[1 + pay[0]:]
            pmt = ((sec[10] & 31) << 8) | sec[11]
        elif pid == pmt and start:
            sec = pay[1 + pay[0]:]
            n = ((sec[1] & 15) << 8) | sec[2]
            k = 12 + (((sec[10] & 15) << 8) | sec[11])
            while k < 3 + n - 4:
                pids[((sec[k + 1] & 31) << 8) | sec[k + 2]] = sec[k]
                k += 5 + (((sec[k + 3] & 15) << 8) | sec[k + 4])
        elif pid in pids:
            if start:
                pes.setdefault(pid, []).append(bytearray(pay[9 + pay[8]:]))
                order.append(pid)
            elif pes.get(pid):
                pes[pid][-1] += pay
    return {pids[k]: v for k, v in pes.items()}


def adts_frames(data):
    out = []
    at = 0
    while at + 7 <= len(data):
        n = ((data[at + 3] & 3) << 11) | (data[at + 4] << 3) | (data[at + 5] >> 5)
        out.append(bytes(data[at:at + n]))
        at += n
    return out


class Bits:
    def __init__(self, b):
        self.b, self.i = b, 0

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.b[self.i >> 3] >> (7 - (self.i & 7))) & 1)
            self.i += 1
        return v

    def ue(self):
        z = 0
        while not self.u(1):
            z += 1
        return (1 << z) - 1 + self.u(z)


def rbsp(nal):
    out, zeros = bytearray(), 0
    for b in nal[1:]:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        zeros = zeros + 1 if b == 0 else 0
        out.append(b)
    return bytes(out)


def access_units(stream):
    """The stream's access units in decoding order, each with its place in
    display order (from its picture order count; the colours' GOPs reset it
    at each IDR)."""
    nals = [n for n in re.split(b"\x00\x00\x00\x01|\x00\x00\x01", stream) if n]
    raw, cur = [], bytearray()
    log2_fn = log2_poc = 0
    for nal in nals:
        t = nal[0] & 31
        if t == 7:
            b = Bits(rbsp(nal))
            b.u(24)
            b.ue()
            log2_fn = b.ue() + 4
            if b.ue() != 0:
                sys.exit("the colours' stream is not picture order type 0")
            log2_poc = b.ue() + 4
        cur += b"\x00\x00\x00\x01" + nal
        if t in (1, 5):
            b = Bits(rbsp(nal))
            b.ue()
            b.ue()
            b.ue()
            b.u(log2_fn)
            if t == 5:
                b.ue()
            raw.append((t == 5, b.u(log2_poc), bytes(cur)))
            cur = bytearray()
    # How far apart neighbouring pictures' counts are: Windows' encoder
    # counts one a picture, others two.
    pocs = sorted(set(p for _, p, _ in raw))
    step = min((b - a for a, b in zip(pocs, pocs[1:])), default=1)
    units, gop_start, shown_before = [], 0, 0
    for idr, poc, au in raw:
        if idr:
            gop_start = shown_before
        idx = gop_start + poc // step
        units.append((idx, au))
        shown_before = max(shown_before, idx + 1)
    if sorted(i for i, _ in units) != list(range(len(units))):
        sys.exit("the colours' pictures do not make one display order")
    return units


def crc32_mpeg(data):
    c = 0xFFFFFFFF
    for b in data:
        c ^= b << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    return c


class Muxer:
    def __init__(self):
        self.cc = {}
        self.out = bytearray()

    def packets(self, pid, payload, start=True, pcr=None):
        first = True
        while payload or first:
            cc = self.cc.get(pid, 0)
            self.cc[pid] = (cc + 1) & 15
            head = bytes([0x47, (0x40 if first and start else 0) | (pid >> 8), pid & 255])
            # The adaptation field: the clock on a packet that asks for it,
            # and stuffing to fill out the last packet.
            fields = b""
            if first and pcr is not None:
                base = pcr & ((1 << 33) - 1)
                fields = bytes([0x10, (base >> 25) & 255, (base >> 17) & 255, (base >> 9) & 255,
                                (base >> 1) & 255, ((base & 1) << 7) | 0x7E, 0])
            room = 184 - (1 + len(fields) if fields else 0)
            if len(payload) >= room:
                adapt = bytes([len(fields)]) + fields if fields else b""
                self.out += head + bytes([(0x30 if fields else 0x10) | cc]) + adapt + payload[:room]
                payload = payload[room:]
            else:
                total = 183 - len(payload)                  # the adaptation field's length
                body = fields if fields else (bytes([0]) if total else b"")
                adapt = bytes([total]) + body + b"\xff" * (total - len(body))
                self.out += head + bytes([0x30 | cc]) + adapt + payload
                payload = b""
            first = False

    def table(self, pid, body):
        sec = bytes(body) + struct.pack(">I", crc32_mpeg(body))
        self.packets(pid, b"\x00" + sec)

    def pes(self, pid, sid, pts, data, dts=None):
        def stamp(kind, t):
            t &= (1 << 33) - 1
            return bytes([kind | ((t >> 29) & 0x0E), (t >> 22) & 255, 0x01 | ((t >> 14) & 0xFE),
                          (t >> 7) & 255, 0x01 | ((t << 1) & 0xFE)])
        if dts is None:
            body = bytes([0x80, 0x80, 5]) + stamp(0x21, pts) + data
        else:
            body = bytes([0x80, 0xC0, 10]) + stamp(0x31, pts) + stamp(0x11, dts) + data
        n = len(body) if len(body) < 65536 and sid == 0xC0 else 0
        self.packets(pid, b"\x00\x00\x01" + bytes([sid]) + struct.pack(">H", n) + body,
                     pcr=(dts * 300 if dts is not None else None))


def segment(k, units, frames):
    """Segment k: the colours' two seconds and four copies' worth of tone
    seconds, times running on from the segments before."""
    m = Muxer()
    m.table(0, bytes([0x00, 0xB0, 13, 0, 1, 0xC1, 0, 0, 0, 1, 0xF0, 0x00]))
    m.table(0x1000, bytes([0x02, 0xB0, 23, 0, 1, 0xC1, 0, 0, 0xE1, 0x00, 0xF0, 0,
                           0x1B, 0xE1, 0x00, 0xF0, 0, 0x0F, 0xE1, 0x01, 0xF0, 0]))
    per = len(units)
    # Decoding runs ahead of showing by the most any picture comes early.
    lead = max(i - idx for i, (idx, _) in enumerate(units))
    for i, (idx, au) in enumerate(units):
        m.pes(0x100, 0xE0, 90000 + (k * per + idx) * FRAME, au, dts=90000 + (k * per + i - lead) * FRAME)
    for n in range(2 * len(frames)):
        g = k * 2 * len(frames) + n
        m.pes(0x101, 0xC0, 90000 + g * 1024 * 90000 // 44100, frames[n % len(frames)])
    return bytes(m.out)


def playlist(seconds):
    # Relative addresses, as Twitch's are, so the player has to join them to
    # the playlist's own.
    lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:3",
             "#EXT-X-MEDIA-SEQUENCE:0"]
    for i in range(SEGMENTS):
        lines += ["#EXTINF:%.3f," % seconds, "seg%d.ts" % i]
    lines.append("#EXT-X-ENDLIST")
    return ("\n".join(lines) + "\n").encode()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    segs = []
    seconds = 2.0

    def log_message(self, *a):
        pass

    def do_GET(self):
        ASKED[self.path] = ASKED.get(self.path, 0) + 1
        m = re.fullmatch(r"/live/seg(\d+)\.ts", self.path)
        if self.path == "/watch.html":
            # All of the page is the link, so a click anywhere on it lands.
            stream = "http://%s/live/index.m3u8" % self.headers.get("Host", "")
            body = ("<!doctype html><html><head><title>watch</title><style>"
                    "body{margin:0}a{display:block;height:3000px;background:#203860;color:#fff}"
                    "</style></head><body><a href=\"play:%s\">watch</a></body></html>" % stream).encode()
            ctype = "text/html"
        elif self.path == "/live/index.m3u8":
            body, ctype = playlist(Handler.seconds), "application/vnd.apple.mpegurl"
        elif m and int(m.group(1)) < SEGMENTS:
            body, ctype = Handler.segs[int(m.group(1))], "video/mp2t"
        else:
            body, ctype = b"no", "text/plain"
            self.send_response(404)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


# --- what came out --------------------------------------------------------------------------

def read_stereo(path):
    """Both channels and the rate; the lengths in the header may be zero (see
    soundcheck's read_wav)."""
    raw = open(path, "rb").read()
    if len(raw) < 44 or raw[0:4] != b"RIFF":
        return 0, [], []
    rate, chans, data, at = 0, 1, b"", 12
    while at + 8 <= len(raw):
        tag = raw[at:at + 4]
        size = struct.unpack("<I", raw[at + 4:at + 8])[0]
        body = at + 8
        if tag == b"fmt ":
            chans = struct.unpack("<H", raw[body + 2:body + 4])[0]
            rate = struct.unpack("<I", raw[body + 4:body + 8])[0]
        elif tag == b"data":
            data = raw[body:] if size == 0 or body + size > len(raw) else raw[body:body + size]
            break
        at = body + size + (size & 1)
    n = len(data) // (2 * chans)
    s = struct.unpack("<%dh" % (n * chans), data[:n * chans * 2])
    left = list(s[0::chans])
    right = list(s[1::chans]) if chans > 1 else left
    return rate, left, right


def pitch_of(samples, rate):
    """Cycles per second from the median time between upward crossings.

    Not soundcheck's count of crossings over a stretch: the encoder's quiet
    lead-in at each second's start loses a few dozen crossings a second,
    which reads as a tone four per cent flat when the sound is right. A
    median of periods does not see a gap at all."""
    gate = max(200, max(abs(v) for v in samples) // 4)
    ups, state = [], 0
    for i, v in enumerate(samples):
        if state <= 0 and v > gate:
            state = 1
            ups.append(i)
        elif state >= 0 and v < -gate:
            state = -1
    periods = sorted(b - a for a, b in zip(ups, ups[1:]))
    if not periods:
        return 0.0
    # Periods are whole samples; averaging the middle half of them keeps the
    # fraction that one median sample would round off.
    mid = periods[len(periods) // 4:3 * len(periods) // 4] or periods
    return rate * len(mid) / float(sum(mid))


def stretches(samples, rate, floor=2000):
    """Where the sound starts and stops, and the longest quiet inside it, in
    five millisecond blocks (a sine crosses zero all the time: a block is
    loud if anything in it is)."""
    block = max(1, rate // 200)
    loud = []
    for start in range(0, len(samples) - block, block):
        loud.append(max(abs(v) for v in samples[start:start + block]) >= floor)
    if True not in loud:
        return None
    first = loud.index(True)
    last = len(loud) - 1 - loud[::-1].index(True)
    gap = run = 0
    for v in loud[first:last + 1]:
        run = 0 if v else run + 1
        gap = max(gap, run)
    return first * block, (last + 1) * block, gap * block


COLOURS = {"red": (255, 0, 0), "blue": (0, 0, 255), "green": (0, 255, 0), "yellow": (255, 255, 0)}


def colours_seen(w, h, px):
    """How many of every fourth pixel each way are near each of the clip's
    colours. Nothing on the desktop is that saturated over that much of it."""
    n = dict.fromkeys(COLOURS, 0)
    for y in range(0, h, 4):
        row = y * w * 3
        for x in range(0, w, 4):
            i = row + x * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            for name, (cr, cg, cb) in COLOURS.items():
                if abs(r - cr) < 48 and abs(g - cg) < 48 and abs(b - cb) < 48:
                    n[name] += 1
    return n


def main():
    build_once()
    c = Checks("play test")
    tone = adts_frames(b"".join(ts_payloads(header_array("AACD_TS", "aacdata.h"))[0x0F]))
    units = access_units(header_array("H264D_COLOURS", "h264data.h"))
    Handler.segs = [segment(k, units, tone) for k in range(SEGMENTS)]
    Handler.seconds = 2 * len(tone) * 1024 / 44100.0

    for stale in (WAV, DISK):
        if os.path.exists(stale):
            os.remove(stale)

    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    page = "http://10.0.2.2:%d/watch.html" % httpd.server_address[1]

    vm = Guest(DISK, memory=256, machine="q35",
               extra=AUDIO + ["-nic", "user,model=e1000"])
    said = ""
    shown = False
    pairs = {"red and blue": 0, "green and yellow": 0}
    looks = 0
    try:
        vm.wait_boot()
        vm.run("dhcp", timeout=25)
        mark = len(vm.serial())
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(*PARK)
        # The browser says each page it has shown on the console. A command
        # typed before the desktop is taking them is lost, so it is typed
        # again until the page is shown.
        for _ in range(4):
            vm.type("browser %s\n" % page)
            if vm.wait_serial("browser: %s -- watch" % page, timeout=45):
                shown = True
                break
        if shown:
            mon.click((PAGE[0] + PAGE[2]) // 2, (PAGE[1] + PAGE[3]) // 2)
            mon.move_to(*PARK)
            # Looking at the screen while it plays, until the player says
            # the stream has ended -- as long as six seconds take a guest
            # that may be running at half speed under a busy host, and then
            # some: the wait is for what it says, not for a time.
            end = time.time() + 180
            while time.time() < end and "play: the stream has ended" not in vm.serial()[mark:]:
                if "play: playing" in vm.serial()[mark:]:
                    w, h, px, _ = mon.screen("play-look")
                    n = colours_seen(w, h, px)
                    looks += 1
                    if n["red"] > 2000 and n["blue"] > 2000:
                        pairs["red and blue"] += 1
                    if n["green"] > 2000 and n["yellow"] > 2000:
                        pairs["green and yellow"] += 1
                else:
                    time.sleep(0.2)
        said = vm.serial()[mark:]
        # The card has a second and a third queued when the queue empties.
        time.sleep(2.5)
    finally:
        try:
            vm.monitor().send("quit", settle=0.5)
        except Exception:
            pass
        time.sleep(1.0)
        vm.stop()
        httpd.shutdown()
        try:
            os.remove(DISK)
        except OSError:
            pass

    c.add("the browser shows a page with a stream's link on it", shown)
    c.add("and a click on the link starts the player, which plays the stream",
          "play: playing" in said)
    c.add("and says when it has ended", "play: the stream has ended" in said)
    c.add("with every segment decoded",
          "could not be decoded" not in said and "could not be fetched" not in said)
    c.add("the playlist is read once, since it says it is finished",
          ASKED.get("/live/index.m3u8") == 1)
    c.add("and each segment fetched once, from the first",
          all(ASKED.get("/live/seg%d.ts" % i) == 1 for i in range(SEGMENTS)))

    m = re.search(r"play: shown (\d+) frames, passed over (\d+)", said)
    frames, passed = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    print("      %d frames shown, %d passed over; %d looks at the screen: %s" % (frames, passed, looks, pairs))
    total = SEGMENTS * len(units)
    c.add("the picture is shown, most of it on time (%d of %d frames)" % (frames, total),
          frames >= total // 2 and frames + passed <= total)
    c.add("red beside blue is seen", pairs["red and blue"] > 0)
    c.add("and green beside yellow", pairs["green and yellow"] > 0)
    # Each pair is half of every segment, so each is on the screen for about
    # half of the looks. A player that showed frames as they were decoded,
    # not when the sound reached them, would race through them and leave the
    # last one up: the first pair hardly seen at all.
    c.add("each for about as long as the other, as the sound keeps time (%d and %d of %d looks)"
          % (pairs["red and blue"], pairs["green and yellow"], looks),
          looks > 0 and min(pairs.values()) * 4 >= looks)

    if not os.path.exists(WAV):
        c.add("something was recorded", False)
        return c.report()
    rate, left, right = read_stereo(WAV)
    c.add("something was recorded", rate > 0 and len(left) > rate)

    found = stretches(left, rate) if rate else None
    if found is None:
        c.add("and it is not silence", False)
    else:
        start, end, gap = found
        secs = (end - start) / float(rate)
        print("      heard %.2fs of sound from %.2fs, the longest quiet in it %d ms"
              % (secs, start / float(rate), gap * 1000 // rate))
        # Six seconds of tone, the encoder's lead-in at the very start quiet
        # and not counted.
        want = SEGMENTS * Handler.seconds
        c.add("all three segments are heard (%.2fs)" % secs, want - 0.5 <= secs <= want + 0.5)
        # The tone fades in under the encoder at the start of each second,
        # a few tens of milliseconds; the card running dry while the next
        # segment was fetched, or while pictures were decoded, would be far
        # longer than that.
        c.add("with no gap while the next segment was fetched (%d ms)"
              % (gap * 1000 // rate), gap * 1000 // rate <= 80)
        low, high = pitch_of(left[start:end], rate), pitch_of(right[start:end], rate)
        c.add("the left is the pitch encoded (440 Hz, heard %d)" % low, close_to(low, 440, 0.03))
        c.add("and the right the other (660 Hz, heard %d)" % high, close_to(high, 660, 0.03))

    try:
        os.remove(WAV)
    except OSError:
        pass
    return c.report()


if __name__ == "__main__":
    sys.exit(main())
