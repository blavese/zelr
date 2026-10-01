"""A page that plays a film the way the players on the web do, by Media
Source Extensions, listened to and looked at.

The page is served here, with its segments: fragmented MP4 holding the two
seconds of flat colours in userland/h264data.h (red beside blue, then green
beside yellow, ten pictures a second) and twice the second of tone in
userland/aacdata.h (440 Hz on the left, 660 Hz on the right), cut into the
initialisation segments and media segments a page appends. Its script does
what a player's does: a MediaSource given to a <video> by its blob: address,
a SourceBuffer for each stream, each segment fetched and appended in turn,
the end of the stream said, then play(). When the element says it has ended
the page goes to /mse-done with every event the element told, in order, and
what it says of itself then; that request is what this reads, so a browser
that never got there fails here rather than passing by saying nothing wrong.

jsmedia.h's events and buffers are checked in pagetest, and the pictures
against Windows' decoding there and in mediatest. This is the rest of the
road, which only a running machine has: the frames drawn in the element's
box on the screen, at the time the sound reaches them, and the sound itself
at the card.

What going wrong would look or sound like:
  no MediaSource, or no SourceBuffer    the page reports an error, or nothing
  the element's events wrong            the report differs
  frames not drawn                      the colours not seen
  frames drawn as decoded, not timed    the first pair hardly seen
  no sound, or the wrong rate           silence, or the wrong pitch

  python tools/msecheck.py [--keep]
  python tools/msecheck.py --serve PORT     (the page and segments, for the host browser)
"""
import http.server
import os
import re
import struct
import sys
import threading
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT                         # noqa: E402
from soundcheck import close_to                                             # noqa: E402
from genmedia import header_array, video_units, sps_size, box, full, init_segment, fragment   # noqa: E402
from playcheck import ts_payloads, adts_frames, read_stereo, pitch_of, stretches, colours_seen  # noqa: E402

DISK = os.path.join(ROOT, "msecheck.%d.img" % os.getpid())
WAV = os.path.join(ROOT, "msecheck.%d.wav" % os.getpid())
AUDIO = ["-audiodev", "wav,id=a0,path=" + WAV.replace("\\", "/"),
         "-device", "intel-hda",
         "-device", "hda-output,audiodev=a0"]

# What the element tells, in order, for a page that appends everything, ends
# the stream and then plays (timeupdates folded into one).
EVENTS = ("loadstart,loadedmetadata,resize,loadeddata,canplay,canplaythrough,durationchange,"
          "play,playing,resolved,timeupdate,pause,ended")


def colour_film():
    """The colours as fragmented MP4: an initialisation segment and a media
    segment for each group of pictures (each starts with one that needs no
    other, as a player's segments do)."""
    sps, pps, units = video_units(header_array("H264D_COLOURS", "h264data.h"))
    w, h = sps_size(sps)
    avcc = bytes([1, sps[1], sps[2], sps[3], 0xFF, 0xE1]) + struct.pack(">H", len(sps)) + sps + \
        bytes([1]) + struct.pack(">H", len(pps)) + pps
    entry = box(b"avc1", b"\0" * 6, struct.pack(">H", 1), b"\0" * 16, struct.pack(">HH", w, h),
                struct.pack(">II", 0x480000, 0x480000), struct.pack(">I", 0), struct.pack(">H", 1), b"\0" * 32,
                struct.pack(">Hh", 0x18, -1), box(b"avcC", avcc))
    timescale, dur = 90000, 9000
    init = init_segment(1, timescale, b"vide", entry, w, h)
    starts = [i for i, (idr, _, _) in enumerate(units) if idr] + [len(units)]
    segs = []
    for n in range(len(starts) - 1):
        samples = []
        for i in range(starts[n], starts[n + 1]):
            idr, idx, au = units[i]
            samples.append((dur, len(au), 0x02000000 if idr else 0x01010000, (idx - i) * dur, au))
        segs.append(fragment(n + 1, 1, starts[n] * dur, samples))
    return init, segs, len(units), w, h


def tone_film():
    """Twice the second of tone, as one initialisation and two media segments."""
    frames = adts_frames(b"".join(ts_payloads(header_array("AACD_TS", "aacdata.h"))[0x0F]))
    h = frames[0]
    sfi = (h[2] >> 2) & 15
    chans = ((h[2] & 1) << 2) | (h[3] >> 6)
    rate = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000][sfi]
    asc = struct.pack(">H", (2 << 11) | (sfi << 7) | (chans << 3))
    dec_specific = bytes([5, len(asc)]) + asc
    dec_config = bytes([4, 13 + len(dec_specific), 0x40, 0x15, 0, 0, 0]) + struct.pack(">II", 128000, 128000) + dec_specific
    es = bytes([3, 3 + len(dec_config) + 3]) + struct.pack(">HB", 0, 0) + dec_config + bytes([6, 1, 2])
    entry = box(b"mp4a", b"\0" * 6, struct.pack(">H", 1), b"\0" * 8, struct.pack(">HHHH", chans, 16, 0, 0),
                struct.pack(">I", rate << 16), full(b"esds", 0, 0, es))
    init = init_segment(2, rate, b"soun", entry)
    raw = [f[7:] for f in frames]
    segs = []
    for n in range(2):
        samples = [(1024, len(a), 0x02000000, 0, a) for a in raw]
        segs.append(fragment(n + 1, 2, n * len(raw) * 1024, samples))
    return init, segs, 2 * len(raw) * 1024 / float(rate)


PAGE = """<!doctype html><html><head><title>film</title><style>
body{margin:0;background:#101010}
video{display:block;width:640px;height:352px;margin:8px}
</style></head><body><video id=v></video><script>
var v = document.getElementById('v'), log = [], ms = new MediaSource();
['loadstart','durationchange','loadedmetadata','loadeddata','canplay','canplaythrough','play','playing',
 'waiting','seeking','seeked','timeupdate','pause','ended','resize','error','emptied','abort'].forEach(function (e) {
  v.addEventListener(e, function () { if (e !== 'timeupdate' || log[log.length - 1] !== 'timeupdate') log.push(e); });
});
function get(u) { return fetch(u).then(function (r) { return r.arrayBuffer(); }); }
function append(sb, u) {
  return get(u).then(function (b) { return new Promise(function (ok) {
    sb.addEventListener('updateend', function f() { sb.removeEventListener('updateend', f); ok(); });
    sb.appendBuffer(b);
  }); });
}
function report(what) { location.href = '/mse-done?' + encodeURIComponent(what); }
ms.addEventListener('sourceopen', function () {
  var vb = ms.addSourceBuffer('video/mp4; codecs="%(VCODEC)s"');
  var ab = ms.addSourceBuffer('audio/mp4; codecs="mp4a.40.2"');
  var chain = append(vb, '/film/v-init').then(function () { return append(ab, '/film/a-init'); });
  %(VSEGS)s.forEach(function (u) { chain = chain.then(function () { return append(vb, u); }); });
  %(ASEGS)s.forEach(function (u) { chain = chain.then(function () { return append(ab, u); }); });
  chain.then(function () {
    ms.endOfStream();
    return v.play();
  }).then(function () { log.push('resolved'); fetch('/mse-note?playing'); },
          function (e) { report('error ' + e.name + ' ' + e.message); });
});
v.addEventListener('ended', function () {
  var q = v.getVideoPlaybackQuality();
  report([log.join(','), v.currentTime.toFixed(2), v.duration.toFixed(2), v.videoWidth, v.videoHeight,
          q.totalVideoFrames, q.droppedVideoFrames].join(' '));
});
v.addEventListener('error', function () { report('element error ' + (v.error && v.error.code)); });
v.src = URL.createObjectURL(ms);
</script></body></html>"""

REPORTS = []
NOTES = []
ASKED = {}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    files = {}

    def log_message(self, *a):
        pass

    def do_GET(self):
        ASKED[self.path.split("?")[0]] = ASKED.get(self.path.split("?")[0], 0) + 1
        if self.path.startswith("/mse-note?"):
            NOTES.append(self.path[len("/mse-note?"):])
            body, ctype = b"noted", "text/plain"
        elif self.path.startswith("/mse-done?"):
            REPORTS.append(urllib.parse.unquote(self.path[len("/mse-done?"):]))
            body, ctype = b"<title>done</title><p>done</p>", "text/html"
        elif self.path in Handler.files:
            body, ctype = Handler.files[self.path]
        else:
            body = b"no"
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


class QuietServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass                # a browser dropping a kept connection is not a fault


def serve(port=0):
    vinit, vsegs, frames, w, h = colour_film()
    ainit, asegs, seconds = tone_film()
    files = {"/film/v-init": (vinit, "video/mp4"), "/film/a-init": (ainit, "audio/mp4")}
    vnames = ["/film/v%d" % i for i in range(len(vsegs))]
    anames = ["/film/a%d" % i for i in range(len(asegs))]
    for n, s in zip(vnames, vsegs):
        files[n] = (s, "video/mp4")
    for n, s in zip(anames, asegs):
        files[n] = (s, "audio/mp4")
    page = PAGE % {"VCODEC": "avc1.%02x%02x%02x" % tuple(vinit[vinit.index(b"avcC") + 5:vinit.index(b"avcC") + 8]),
                   "VSEGS": "[" + ",".join("'%s'" % n for n in vnames) + "]",
                   "ASEGS": "[" + ",".join("'%s'" % n for n in anames) + "]"}
    files["/mse.html"] = (page.encode(), "text/html")
    Handler.files = files
    httpd = QuietServer(("127.0.0.1", port), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, frames, w, h, seconds


def main():
    if "--serve" in sys.argv:
        httpd, frames, w, h, seconds = serve(int(sys.argv[sys.argv.index("--serve") + 1]))
        print("http://127.0.0.1:%d/mse.html (%d pictures %dx%d, %.2fs of sound)"
              % (httpd.server_address[1], frames, w, h, seconds), flush=True)
        while True:
            time.sleep(1)
            while REPORTS:
                print("report:", REPORTS.pop(0), flush=True)
            while NOTES:
                print("note:", NOTES.pop(0), flush=True)
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("a page's film, by Media Source Extensions")
    for stale in (WAV, DISK):
        if os.path.exists(stale):
            os.remove(stale)
    httpd, frames, fw, fh, seconds = serve()
    page = "http://10.0.2.2:%d/mse.html" % httpd.server_address[1]
    vm = Guest(DISK, memory=256, machine="q35", extra=AUDIO + ["-nic", "user,model=e1000"])
    shown = False
    said = ""
    pairs = {"red and blue": 0, "green and yellow": 0}
    looks = 0
    try:
        vm.wait_boot()
        vm.run("dhcp", timeout=25)
        mark = len(vm.serial())
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 740)
        for _ in range(4):
            vm.type("browser %s\n" % page)
            if vm.wait_serial("browser: %s -- film" % page, timeout=45):
                shown = True
                break
        if shown:
            # Looking at the screen while it plays, until the page reports:
            # a wait for what it says, not for a time.
            end = time.time() + 180
            while time.time() < end and not REPORTS:
                if "playing" in NOTES:
                    w, h, px, _ = mon.screen("mse-look")
                    n = colours_seen(w, h, px)
                    looks += 1
                    if n["red"] > 2000 and n["blue"] > 2000:
                        pairs["red and blue"] += 1
                    if n["green"] > 2000 and n["yellow"] > 2000:
                        pairs["green and yellow"] += 1
                else:
                    time.sleep(0.2)
        said = vm.serial()[mark:]
        time.sleep(2.0)
    finally:
        try:
            vm.monitor().send("quit", settle=0.5)
        except Exception:
            pass
        time.sleep(1.0)
        vm.stop()
        httpd.shutdown()
        if not keep:
            try:
                os.remove(DISK)
            except OSError:
                pass

    c.add("the browser shows the page", shown)
    c.add("its script appends every segment and the element plays", "playing" in NOTES)
    got = REPORTS[0] if REPORTS else ""
    print("      the page said: %s" % got)
    parts = got.split(" ")
    c.add("the element tells what the standard has it tell, in order", parts[:1] == [EVENTS])
    c.add("and at the end it is at the end, with the picture's size",
          len(parts) >= 5 and parts[1] == parts[2] and parts[3:5] == [str(fw), str(fh)])
    shown_frames = int(parts[5]) - int(parts[6]) if len(parts) >= 7 else 0
    print("      %d of %d frames shown; %d looks at the screen: %s" % (shown_frames, frames, looks, pairs))
    c.add("most of the pictures are shown (%d of %d)" % (shown_frames, frames), shown_frames >= frames // 2)
    c.add("red beside blue is seen in the element's box", pairs["red and blue"] > 0)
    c.add("and green beside yellow", pairs["green and yellow"] > 0)
    # Each pair is half the film. Shown as decoded rather than as the sound
    # reaches them, the first would hardly be seen.
    c.add("each for about as long as the other (%d and %d of %d looks)"
          % (pairs["red and blue"], pairs["green and yellow"], looks),
          looks > 0 and min(pairs.values()) * 4 >= looks)
    c.add("every segment was fetched once", all(v == 1 for k, v in ASKED.items() if k.startswith("/film/")))

    if not os.path.exists(WAV):
        c.add("something was recorded", False)
        return c.report(keep=keep)
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
        c.add("the whole of the sound is heard (%.2fs of %.2fs)" % (secs, seconds), seconds - 0.4 <= secs <= seconds + 0.4)
        c.add("with no gap in it (%d ms)" % (gap * 1000 // rate), gap * 1000 // rate <= 80)
        low, high = pitch_of(left[start:end], rate), pitch_of(right[start:end], rate)
        c.add("the left is the pitch encoded (440 Hz, heard %d)" % low, close_to(low, 440, 0.03))
        c.add("and the right the other (660 Hz, heard %d)" % high, close_to(high, 660, 0.03))
    if not keep:
        try:
            os.remove(WAV)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
