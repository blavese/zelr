"""Makes userland/h264data.h: H.264 streams made by Windows' own encoder from
clips made here, and what Windows' own decoder makes of them, for h264test.

Windows is reached through tools/mfh264.c (Media Foundation's sink writer and
source reader), built here with zig as a host tool. The decoder's answers
are kept as a checksum of each plane of each frame, in display order:
decoding H.264 is exact, so one sample wrong is a different sum.

The streams cover CAVLC and CABAC, P and B pictures (with spatial direct
prediction, which is what Windows' encoder uses), reordering, a picture size
that is cropped, and several GOPs. The 8x8 transform, temporal direct
prediction and weighted prediction are not in them, because Windows'
encoder never uses those; tools/h264check.py checks them against live
streams, which do.

Also a clip of flat colours, for tools/playcheck.py to look for on the
screen.

  python tools/genh264.py
"""
import math
import os
import random
import struct
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
BUILD = os.path.join(ROOT, "build", "host")
MF = os.path.join(BUILD, "mfh264.exe")
OUT = os.path.join(ROOT, "userland", "h264data.h")
TMP = os.path.join(ROOT, "build", "genh264")


def build_mf():
    os.makedirs(BUILD, exist_ok=True)
    r = subprocess.run(["zig", "cc", "-O1", "-target", "x86_64-windows-gnu", os.path.join(TOOLS, "mfh264.c"),
                        "-o", MF, "-lmfplat", "-lmfreadwrite", "-lole32", "-loleaut32"])
    if r.returncode:
        sys.exit("could not build tools/mfh264.c")


# --- clips -------------------------------------------------------------------------------------

def shapes(W, H, N):
    """Shapes moving different ways over a gradient."""
    out = bytearray()
    for t in range(N):
        Y = bytearray(W * H)
        for y in range(H):
            for x in range(W):
                v = (x * 2 + y + t * 3) & 255
                cx, cy = 40 + t * 3, 60 + int(20 * math.sin(t / 4))
                if (x - cx) ** 2 + (y - cy) ** 2 < 400:
                    v = 230 - ((x + y) & 31)
                if 100 <= x < 140 and 20 + t <= y < 50 + t:
                    v = 30 + ((x * y) & 15)
                Y[y * W + x] = v
        U = bytearray(((x + t) * 3 & 255) for y in range(H // 2) for x in range(W // 2))
        V = bytearray(((y * 2 - t) & 255) for y in range(H // 2) for x in range(W // 2))
        out += Y + U + V
    return bytes(out)


def busy(W, H, N):
    """A panning texture with noise, a ball, a box and a ring, and a flash
    half way (intra blocks in predicted pictures)."""
    rnd = random.Random(7)
    tex = [[(int(128 + 60 * math.sin(x / 7.0) * math.cos(y / 5.0)) + rnd.randint(-20, 20)) & 255
            for x in range(W * 2)] for y in range(H * 2)]
    out = bytearray()
    for t in range(N):
        Y = bytearray(W * H)
        for y in range(H):
            row = tex[(y + t) % (H * 2)]
            for x in range(W):
                v = row[(x + t * 3) % (W * 2)]
                cx, cy = 20 + t * 4, H // 2 + int(30 * math.sin(t / 5.0))
                if (x - cx) ** 2 + (y - cy) ** 2 < 300:
                    v = 220 - ((x * 3 + y) & 15)
                bx, by = W - 60 - t * 2, 10 + t * 3
                if bx <= x < bx + 40 and by <= y < by + 30:
                    v = 40 + ((x ^ y) & 31)
                rx, ry = W // 2 + int(50 * math.cos(t / 6.0)), H // 3 + int(20 * math.sin(t / 6.0))
                if 150 < (x - rx) ** 2 + (y - ry) ** 2 < 400:
                    v = 255 - v
                if t == N // 2:
                    v = min(255, v + 80)
                Y[y * W + x] = v
        U = bytearray(((x * 2 + t * 5) & 255) for y in range(H // 2) for x in range(W // 2))
        V = bytearray(((y * 3 - t * 2 + (x >> 2)) & 255) for y in range(H // 2) for x in range(W // 2))
        out += Y + U + V
    return bytes(out)


# Flat colours, a second each, left half and right half: what playcheck
# looks for on the screen. (Y, U, V) for red, blue, green and yellow at
# BT.601 limited range.
RED, BLUE, GREEN, YELLOW = (81, 90, 240), (41, 240, 110), (145, 54, 34), (210, 16, 146)
COLOUR_SECONDS = [(RED, BLUE), (GREEN, YELLOW)]


def colours(W, H, fps):
    out = bytearray()
    for left, right in COLOUR_SECONDS:
        for _ in range(fps):
            for p, s in ((0, 1), (1, 2), (2, 2)):
                w, h = W // s, H // s
                row = bytes([left[p]] * (w // 2) + [right[p]] * (w - w // 2))
                out += row * h
    return bytes(out)


# --- MP4 to Annex B ------------------------------------------------------------------------------

def boxes(buf, start, end):
    at = start
    while at + 8 <= end:
        size, kind = struct.unpack(">I4s", buf[at:at + 8])
        head = 8
        if size == 1:
            size = struct.unpack(">Q", buf[at + 8:at + 16])[0]
            head = 16
        elif size == 0:
            size = end - at
        yield kind.decode("latin-1"), at + head, at + size
        at += size


def find(buf, start, end, path):
    for kind, b, e in boxes(buf, start, end):
        if kind == path[0]:
            if len(path) == 1:
                return b, e
            got = find(buf, b, e, path[1:])
            if got:
                return got
    return None


def annexb(path):
    """The track as Annex B, and its slice types in decoding order."""
    d = open(path, "rb").read()
    trak = find(d, 0, len(d), ["moov", "trak"])
    stbl = find(d, trak[0], trak[1], ["mdia", "minf", "stbl"])
    sb, se = find(d, stbl[0], stbl[1], ["stsd"])
    c = d[d.find(b"avcC", sb, se) + 4:se]
    nl = (c[4] & 3) + 1
    out = bytearray()
    p = 6
    for _ in range(c[5] & 31):
        n = struct.unpack(">H", c[p:p + 2])[0]
        out += b"\0\0\0\1" + c[p + 2:p + 2 + n]
        p += 2 + n
    for _ in range(c[p]):
        n = struct.unpack(">H", c[p + 1:p + 3])[0]
        out += b"\0\0\0\1" + c[p + 3:p + 3 + n]
        p += 2 + n
    b, e = find(d, stbl[0], stbl[1], ["stsz"])
    fixed, count = struct.unpack(">II", d[b + 4:b + 12])
    sizes = [fixed] * count if fixed else list(struct.unpack(">%dI" % count, d[b + 12:b + 12 + 4 * count]))
    got = find(d, stbl[0], stbl[1], ["stco"])
    if got:
        n = struct.unpack(">I", d[got[0] + 4:got[0] + 8])[0]
        chunks = list(struct.unpack(">%dI" % n, d[got[0] + 8:got[0] + 8 + 4 * n]))
    else:
        got = find(d, stbl[0], stbl[1], ["co64"])
        n = struct.unpack(">I", d[got[0] + 4:got[0] + 8])[0]
        chunks = list(struct.unpack(">%dQ" % n, d[got[0] + 8:got[0] + 8 + 8 * n]))
    b, e = find(d, stbl[0], stbl[1], ["stsc"])
    n = struct.unpack(">I", d[b + 4:b + 8])[0]
    runs = [struct.unpack(">III", d[b + 8 + 12 * i:b + 20 + 12 * i]) for i in range(n)]
    samples, si = [], 0
    for ci, off in enumerate(chunks):
        per = 0
        for first, spc, _ in runs:
            if ci + 1 >= first:
                per = spc
        for _ in range(per):
            if si < len(sizes):
                samples.append((off, sizes[si]))
                off += sizes[si]
                si += 1
    types = ""
    for off, size in samples:
        q = off
        while q < off + size:
            n = int.from_bytes(d[q:q + nl], "big")
            nal = d[q + nl:q + nl + n]
            out += b"\0\0\0\1" + nal
            if nal and (nal[0] & 31) in (1, 5):
                bits = "".join(format(x, "08b") for x in nal[1:8])
                i, vals = 0, []
                for _ in range(2):
                    z = 0
                    while bits[i] == "0":
                        z += 1
                        i += 1
                    vals.append(int(bits[i:i + z + 1], 2) - 1)
                    i += z + 1
                types += "PBI"[vals[1] % 5] if vals[1] % 5 < 3 else "?"
            q += nl + n
    return bytes(out), types


def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


# name, clip, width, height, profile, B pictures, GOP, QP
CASES = [
    ("baseline, CAVLC, I and P", "shapes", 176, 144, "base", 0, 15, 26),
    ("main, CABAC, I and P", "shapes", 176, 144, "main", 0, 15, 26),
    ("main, CABAC, B pictures and reordering", "shapes", 176, 144, "main", 2, 15, 26),
    ("high, B pictures, a cropped size, several GOPs", "busy", 320, 180, "high", 2, 8, 32),
]


def main():
    build_mf()
    os.makedirs(TMP, exist_ok=True)
    clips = {}
    o = ["/* Generated by tools/genh264.py: streams made by Windows' own H.264 encoder",
         "   (Media Foundation) from clips made there, and a checksum of each plane of",
         "   each frame Windows' own decoder made of them. Do not edit. */",
         "#pragma once", "",
         "typedef struct {",
         "    const char *name;",
         "    const unsigned char *data; int len;",
         "    int frames, width, height;",
         "    const unsigned int *sums;              /* Y, U, V of each frame, in display order */",
         "    const char *types;                     /* the slice types, in decoding order */",
         "} h264_case;", ""]
    entries = []
    for k, (name, clip, W, H, prof, bf, gop, qp) in enumerate(CASES):
        key = (clip, W, H)
        if key not in clips:
            frames = 30 if clip == "shapes" else 24
            clips[key] = (shapes if clip == "shapes" else busy)(W, H, frames)
        raw = os.path.join(TMP, "clip%d.yuv" % k)
        open(raw, "wb").write(clips[key])
        mp4 = os.path.join(TMP, "case%d.mp4" % k)
        r = subprocess.run([MF, "encode", raw, str(W), str(H), "30", prof, str(bf), str(gop), str(qp), mp4],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if r.returncode or r.stderr.strip():
            sys.exit("Windows would not encode %s: %s" % (name, r.stderr.strip()))
        stream, types = annexb(mp4)
        yuv = os.path.join(TMP, "case%d.yuv" % k)
        r = subprocess.run([MF, "decode", mp4, yuv], stdout=subprocess.PIPE, text=True)
        if r.returncode:
            sys.exit("Windows would not decode %s" % name)
        got = open(yuv, "rb").read()
        fs = W * H * 3 // 2
        if len(got) != fs * len(types):
            sys.exit("%s: Windows gave %d bytes of frames for %d pictures" % (name, len(got), len(types)))
        sums = []
        for f in range(len(types)):
            fr = got[f * fs:(f + 1) * fs]
            sums += [fnv(fr[:W * H]), fnv(fr[W * H:W * H * 5 // 4]), fnv(fr[W * H * 5 // 4:])]
        if bf and "B" not in types:
            sys.exit("%s has no B pictures: %s" % (name, types))
        print("%s: %d bytes, %s" % (name, len(stream), types))
        o.append("static const unsigned char H264D_%d[%d] = {" % (k, len(stream)))
        for i in range(0, len(stream), 24):
            o.append("    " + ",".join(str(b) for b in stream[i:i + 24]) + ",")
        o.append("};")
        o.append("static const unsigned int H264D_SUMS_%d[%d] = {" % (k, len(sums)))
        for i in range(0, len(sums), 6):
            o.append("    " + ", ".join("0x%08x" % v for v in sums[i:i + 6]) + ",")
        o.append("};")
        entries.append('    { "%s", H264D_%d, %d, %d, %d, %d, H264D_SUMS_%d, "%s" },'
                       % (name, k, len(stream), len(types), W, H, k, types))
    o.append("static const h264_case H264_CASES[%d] = {" % len(entries))
    o += entries
    o.append("};")

    # The colours clip, for playcheck: a stream and nothing to compare.
    W, H = 320, 176
    raw = os.path.join(TMP, "colours.yuv")
    # Ten a second, slower than the player could show them if it did not
    # wait: playcheck tells waiting for the sound from not waiting by it.
    open(raw, "wb").write(colours(W, H, 10))
    mp4 = os.path.join(TMP, "colours.mp4")
    r = subprocess.run([MF, "encode", raw, str(W), str(H), "10", "main", "2", "10", "30", mp4],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode or r.stderr.strip():
        sys.exit("Windows would not encode the colours: %s" % r.stderr.strip())
    stream, types = annexb(mp4)
    print("colours: %d bytes, %s" % (len(stream), types))
    o.append("")
    o.append("/* Two seconds at 10 a second, 320x176: red beside blue, then green beside")
    o.append("   yellow (tools/playcheck.py looks for them). */")
    o.append("static const unsigned char H264D_COLOURS[%d] = {" % len(stream))
    for i in range(0, len(stream), 24):
        o.append("    " + ",".join(str(b) for b in stream[i:i + 24]) + ",")
    o.append("};")
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(o) + "\n")
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
