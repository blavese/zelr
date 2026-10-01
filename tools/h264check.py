"""The H.264 decoder against live streams, which Windows' own encoder cannot
make: the 8x8 transform and its intra prediction, temporal direct prediction,
weighted prediction, several references, the encoders real streams come
from. A host check, and an outward one (like sitecheck): it needs the
network and a live channel, and fails without them.

It asks Twitch for its live channels the way its own directory does, takes
the newest segment of every rendition with a picture from the first channel
that answers, and decodes each two ways: by Windows (Media Foundation,
through tools/mfh264.c) and by zelr's decoder (userland/h264.h, built for
the host by tools/h264native.c). Decoding is exact, so every frame must be
the same to the last sample. What each stream used is printed, so a pass
says what it covered.

Nothing fetched is kept: the segments are in build/h264check and replaced
each run.

  python tools/h264check.py [channel]
"""
import json
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
from harness import Checks                                # noqa: E402

BUILD = os.path.join(ROOT, "build", "host")
TMP = os.path.join(ROOT, "build", "h264check")
CLIENT = "kimne78kx3ncx6brgo4mv6wki5h1ko"


def build():
    os.makedirs(BUILD, exist_ok=True)
    mf = os.path.join(BUILD, "mfh264.exe")
    native = os.path.join(BUILD, "h264native.exe")
    r1 = subprocess.run(["zig", "cc", "-O1", "-target", "x86_64-windows-gnu", os.path.join(TOOLS, "mfh264.c"),
                         "-o", mf, "-lmfplat", "-lmfreadwrite", "-lole32", "-loleaut32"])
    r2 = subprocess.run(["zig", "cc", "-O2", "-Wno-unused-function", os.path.join(TOOLS, "h264native.c"),
                         "-I", os.path.join(ROOT, "userland"), "-o", native])
    return (mf if not r1.returncode else None), (native if not r2.returncode else None)


def get(url, data=None, headers=None):
    req = urllib.request.Request(url, data=data, headers=headers or {})
    try:
        return urllib.request.urlopen(req, timeout=30).read()
    except Exception:
        # Python's own certificate store can lack an intermediate; curl
        # uses the system's, verified all the same.
        cmd = ["curl", "-sSfL", "--max-time", "30", url]
        for k, v in (headers or {}).items():
            cmd += ["-H", "%s: %s" % (k, v)]
        if data is not None:
            cmd += ["--data-binary", "@-"]
        r = subprocess.run(cmd, input=data, stdout=subprocess.PIPE)
        if r.returncode:
            raise
        return r.stdout


def gql(query):
    return json.loads(get("https://gql.twitch.tv/gql", json.dumps({"query": query}).encode(),
                          {"Client-Id": CLIENT, "Content-Type": "text/plain;charset=UTF-8"}))


def live_channels():
    r = gql("query{streams(first:8){edges{node{broadcaster{login}}}}}")
    return [e["node"]["broadcaster"]["login"] for e in r["data"]["streams"]["edges"] if e["node"]["broadcaster"]]


def renditions(login):
    q = ('query{streamPlaybackAccessToken(channelName:"%s", params:{platform:"web",playerBackend:"mediaplayer",'
         'playerType:"site"}){value signature}}' % login)
    tok = gql(q)["data"]["streamPlaybackAccessToken"]
    if not tok:
        return []
    url = ("https://usher.ttvnw.net/api/channel/hls/%s.m3u8?allow_source=true&allow_audio_only=true"
           "&player=twitchweb&p=4242&sig=%s&token=%s"
           % (login, tok["signature"], urllib.parse.quote(tok["value"], safe="")))
    master = get(url).decode()
    out = []
    lines = master.splitlines()
    for i, l in enumerate(lines):
        if l.startswith("#EXT-X-STREAM-INF") and "avc1" in l:
            name = re.search(r'VIDEO="([^"]+)"', l)
            res = re.search(r"RESOLUTION=(\d+)x(\d+)", l)
            if name and res:
                out.append((name.group(1), int(res.group(1)), int(res.group(2)), lines[i + 1]))
    return out


def ts_video(d):
    """The H.264 elementary stream of a transport stream."""
    pmt, vpid, out = None, None, bytearray()
    for at in range(0, len(d) - 187, 188):
        p = d[at:at + 188]
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
                if sec[k] == 0x1B:
                    vpid = ((sec[k + 1] & 31) << 8) | sec[k + 2]
                k += 5 + (((sec[k + 3] & 15) << 8) | sec[k + 4])
        elif pid == vpid:
            out += pay[9 + pay[8]:] if start else pay
    return bytes(out)


def main():
    c = Checks("h.264 against live streams")
    mf, native = build()
    c.add("Windows' decoder and zelr's are built for the host", mf is not None and native is not None)
    if not mf or not native:
        return c.report()
    os.makedirs(TMP, exist_ok=True)
    channels = sys.argv[1:] or []
    if not channels:
        try:
            channels = live_channels()
        except Exception as e:
            print("      Twitch's directory did not answer: %s" % e)
    picked, rends = None, []
    for ch in channels:
        try:
            rends = renditions(ch)
        except Exception:
            rends = []
        if rends:
            picked = ch
            break
    c.add("a live channel is found, with renditions to fetch", bool(rends))
    if not rends:
        return c.report()
    print("      channel %s: %s" % (picked, ", ".join(r[0] for r in rends)))
    covered = set()
    for name, w, h, uri in rends:
        try:
            media = get(uri).decode()
            segs = [l for l in media.splitlines() if l and not l.startswith("#")]
            seg = segs[-1]
            data = get(seg if seg.startswith("http") else uri.rsplit("/", 1)[0] + "/" + seg)
        except Exception as e:
            c.add("%s: a segment is fetched" % name, False)
            print("      %s" % e)
            continue
        ts = os.path.join(TMP, "seg.ts")
        es = os.path.join(TMP, "seg.264")
        theirs = os.path.join(TMP, "theirs.yuv")
        ours = os.path.join(TMP, "ours.yuv")
        open(ts, "wb").write(data)
        open(es, "wb").write(ts_video(data))
        for f in (theirs, ours):
            if os.path.exists(f):
                os.remove(f)
        r1 = subprocess.run([mf, "decode", ts, theirs], stdout=subprocess.PIPE, text=True)
        r2 = subprocess.run([native, es, ours], stdout=subprocess.PIPE, text=True)
        m1 = re.search(r"frames (\d+) size (\d+) (\d+)", r1.stdout)
        report = r2.stdout.strip().splitlines()
        info = report[-1] if report else ""
        uses = [l[6:] for l in report if l.startswith("uses: ")]
        for u in uses:
            covered.update(x.strip() for x in u.split(",") if x.strip())
        if not m1:
            c.add("%s: Windows decodes it" % name, False)
            continue
        n, fw, fh = int(m1.group(1)), int(m1.group(2)), int(m1.group(3))
        a = open(theirs, "rb").read() if os.path.exists(theirs) else b""
        b = open(ours, "rb").read() if os.path.exists(ours) else b""
        fs = fw * fh * 3 // 2
        first_bad = next((k for k in range(min(len(a), len(b)) // fs) if a[k * fs:(k + 1) * fs] != b[k * fs:(k + 1) * fs]), -1)
        print("      %s %dx%d: %d frames; zelr: %s; %s" % (name, fw, fh, n, info, "; ".join(uses)))
        c.add("%s: every frame is Windows' decoding of it (%d frames)" % (name, n),
              n > 0 and len(a) == len(b) and first_bad < 0 and a == b)
    print("      covered: %s" % ", ".join(sorted(covered)))
    return c.report()


if __name__ == "__main__":
    sys.exit(main())
