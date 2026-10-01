"""Plays a stream and listens to what came out.

The player (userland/play.c) is the whole road a Twitch channel's sound
takes: a playlist and its segments fetched over HTTP (hls.h), the transport
stream taken apart (ts.h), the AAC decoded (aac.h), and the samples handed to
the card only as fast as it has room for them (sound_info's room). aactest
checks the decoder against Windows' answers; this checks the rest by ear, the
way soundcheck checks a note. A server on the host plays the part of
Twitch's, with a finished playlist of six segments, each the second of tone
in userland/aacdata.h that Windows encoded (440 Hz on the left, 660 Hz on the
right), and QEMU writes down what the card played.

It is started the way a person starts it: the browser, on the desktop, shows
a page from the same server that is one big `play:` link, as a live
channel's "listen" is, and the link is clicked. So the browser handing the
address to the player is checked too, and the sound has to keep up with a
desktop and a browser running beside it.

What going wrong would sound like:
  nothing decoded or written        silence
  the rate converted wrongly         the wrong pitch
  the card left to run dry           gaps between segments
  the end of the list mishandled     less than six seconds, or no end said

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

SEGMENTS = 6
AUDIO = ["-audiodev", "wav,id=a0,path=" + WAV.replace("\\", "/"),
         "-device", "intel-hda",
         "-device", "hda-output,audiodev=a0"]

ASKED = {}


def stream_bytes():
    """The transport stream aactest checks, read out of the header it is in,
    so there is one copy of it."""
    text = open(os.path.join(ROOT, "userland", "aacdata.h"), encoding="ascii").read()
    m = re.search(r"AACD_TS\[(\d+)\] = \{(.*?)\};", text, re.S)
    data = bytes(int(v) for v in re.findall(r"\d+", m.group(2)))
    if len(data) != int(m.group(1)):
        sys.exit("aacdata.h's AACD_TS is not the length it says")
    return data


def playlist():
    # Relative addresses, as Twitch's are, so the player has to join them to
    # the playlist's own.
    lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2",
             "#EXT-X-MEDIA-SEQUENCE:0"]
    for i in range(SEGMENTS):
        lines += ["#EXTINF:1.021,", "seg%d.ts" % i]
    lines.append("#EXT-X-ENDLIST")
    return ("\n".join(lines) + "\n").encode()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    ts = b""

    def log_message(self, *a):
        pass

    def do_GET(self):
        ASKED[self.path] = ASKED.get(self.path, 0) + 1
        if self.path == "/listen.html":
            # All of the page is the link, so a click anywhere on it lands.
            stream = "http://%s/live/index.m3u8" % self.headers.get("Host", "")
            body = ("<!doctype html><html><head><title>listen</title><style>"
                    "body{margin:0}a{display:block;height:3000px;background:#203860;color:#fff}"
                    "</style></head><body><a href=\"play:%s\">listen</a></body></html>" % stream).encode()
            ctype = "text/html"
        elif self.path == "/live/index.m3u8":
            body, ctype = playlist(), "application/vnd.apple.mpegurl"
        elif re.fullmatch(r"/live/seg[0-%d]\.ts" % (SEGMENTS - 1), self.path):
            body, ctype = Handler.ts, "video/mp2t"
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
    lead-in at each segment's start loses a few dozen crossings a second,
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


def main():
    build_once()
    c = Checks("play test")
    Handler.ts = stream_bytes()

    for stale in (WAV, DISK):
        if os.path.exists(stale):
            os.remove(stale)

    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    page = "http://10.0.2.2:%d/listen.html" % httpd.server_address[1]

    vm = Guest(DISK, memory=256, machine="q35",
               extra=AUDIO + ["-nic", "user,model=e1000"])
    said = ""
    shown = False
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
            if vm.wait_serial("browser: %s -- listen" % page, timeout=45):
                shown = True
                break
        if shown:
            mon.click((PAGE[0] + PAGE[2]) // 2, (PAGE[1] + PAGE[3]) // 2)
            mon.move_to(*PARK)
            # As long as six seconds of sound take a guest that may be
            # running at half speed under a busy host, and then some: the
            # wait is for what it says, not for a time.
            vm.wait_serial("play: the stream has ended", timeout=180)
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
        # Each segment is 1.021 s; the encoder's lead-in at the very start is
        # quiet and not counted.
        c.add("all six segments are heard (%.2fs)" % secs, 5.6 <= secs <= 6.6)
        # The tone fades in under the encoder at the start of each copy, a
        # few tens of milliseconds; the card running dry while the next
        # segment was fetched would be far longer than that.
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
