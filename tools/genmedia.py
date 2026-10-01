"""Makes userland/mediadata.h: streams the tests already know, put into
fragmented MP4 the way a page's player appends them to a SourceBuffer
(Media Source Extensions): an initialisation segment, then media segments.

  - h264data.h's "main, CABAC, B pictures and reordering" stream, in three
    fragments of ten pictures, with each picture's composition offset from
    its picture order count. mediatest compares what comes out with the
    same checksums of Windows' decoding that h264test uses.
  - aacdata.h's second of tone as AAC in MP4, in two fragments.

Windows decodes each file made here (Media Foundation, through
tools/mfh264.c), so they are checked by a reader that is not ours before
they are written.

  python tools/genmedia.py
"""
import os
import re
import struct
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
OUT = os.path.join(ROOT, "userland", "mediadata.h")
TMP = os.path.join(ROOT, "build", "genmedia")
MF = os.path.join(ROOT, "build", "host", "mfh264.exe")


def header_array(name, header):
    text = open(os.path.join(ROOT, "userland", header), encoding="ascii").read()
    m = re.search(name + r"\[(\d+)\] = \{(.*?)\};", text, re.S)
    return bytes(int(v) for v in re.findall(r"\d+", m.group(2)))


def header_sums(k):
    text = open(os.path.join(ROOT, "userland", "h264data.h"), encoding="ascii").read()
    m = re.search(r"H264D_SUMS_%d\[\d+\] = \{(.*?)\};" % k, text, re.S)
    return [int(v, 16) for v in re.findall(r"0x[0-9a-f]+", m.group(1))]


def box(kind, *parts):
    body = b"".join(parts)
    return struct.pack(">I4s", 8 + len(body), kind) + body


def full(kind, version, flags, *parts):
    return box(kind, struct.pack(">I", (version << 24) | flags), *parts)


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


def video_units(stream):
    """SPS, PPS, and each picture's NAL units (length-prefixed) with its place
    in display order, in decoding order."""
    nals = [n for n in re.split(b"\x00\x00\x00\x01|\x00\x00\x01", stream) if n]
    sps = pps = None
    raw, cur = [], bytearray()
    log2_fn = log2_poc = 0
    for nal in nals:
        t = nal[0] & 31
        if t == 7:
            sps = nal
            b = Bits(rbsp(nal))
            b.u(24)
            b.ue()
            log2_fn = b.ue() + 4
            if b.ue() != 0:
                sys.exit("the stream is not picture order type 0")
            log2_poc = b.ue() + 4
            continue
        if t == 8:
            pps = nal
            continue
        if t == 9:
            continue
        cur += struct.pack(">I", len(nal)) + nal
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
    pocs = sorted(set(p for _, p, _ in raw))
    step = min((b - a for a, b in zip(pocs, pocs[1:])), default=1)
    units, gop_start, shown_before = [], 0, 0
    for idr, poc, au in raw:
        if idr:
            gop_start = shown_before
        idx = gop_start + poc // step
        units.append((idr, idx, au))
        shown_before = max(shown_before, idx + 1)
    return sps, pps, units


def sps_size(sps):
    b = Bits(rbsp(sps))
    profile = b.u(8)
    b.u(16)
    b.ue()
    if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128):
        sys.exit("a High profile SPS: not needed here")
    b.ue()
    if b.ue() == 0:
        b.ue()
    b.ue()
    b.u(1)
    w = (b.ue() + 1) * 16
    h = (b.ue() + 1) * 16
    return w, h


def init_segment(track_id, timescale, handler, sample_entry, width=0, height=0):
    mvhd = full(b"mvhd", 0, 0, struct.pack(">IIII", 0, 0, 1000, 0), struct.pack(">IH", 0x00010000, 0x0100),
                b"\0" * 10, struct.pack(">9I", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000), b"\0" * 24,
                struct.pack(">I", track_id + 1))
    tkhd = full(b"tkhd", 0, 7, struct.pack(">IIIII", 0, 0, track_id, 0, 0), b"\0" * 8,
                struct.pack(">hhhh", 0, 0, 0x0100 if handler == b"soun" else 0, 0),
                struct.pack(">9I", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000),
                struct.pack(">II", width << 16, height << 16))
    mdhd = full(b"mdhd", 0, 0, struct.pack(">IIII", 0, 0, timescale, 0), struct.pack(">HH", 0x55C4, 0))
    hdlr = full(b"hdlr", 0, 0, struct.pack(">I4s", 0, handler), b"\0" * 12, b"zelr\0")
    media_header = full(b"vmhd", 0, 1, b"\0" * 8) if handler == b"vide" else full(b"smhd", 0, 0, b"\0" * 4)
    dinf = box(b"dinf", full(b"dref", 0, 0, struct.pack(">I", 1), full(b"url ", 0, 1)))
    stbl = box(b"stbl", full(b"stsd", 0, 0, struct.pack(">I", 1), sample_entry),
               full(b"stts", 0, 0, struct.pack(">I", 0)), full(b"stsc", 0, 0, struct.pack(">I", 0)),
               full(b"stsz", 0, 0, struct.pack(">II", 0, 0)), full(b"stco", 0, 0, struct.pack(">I", 0)))
    minf = box(b"minf", media_header, dinf, stbl)
    trak = box(b"trak", tkhd, box(b"mdia", mdhd, hdlr, minf))
    mvex = box(b"mvex", full(b"trex", 0, 0, struct.pack(">IIIII", track_id, 1, 0, 0, 0)))
    ftyp = box(b"ftyp", b"iso5", struct.pack(">I", 0), b"iso5iso6mp41")
    return ftyp + box(b"moov", mvhd, trak, mvex)


def fragment(seq, track_id, base_dts, samples):
    """samples: (duration, size, flags, composition offset, data)."""
    def build(data_offset):
        entries = b"".join(struct.pack(">IIIi", d, s, f, c) for d, s, f, c, _ in samples)
        trun = full(b"trun", 1, 0x000F01, struct.pack(">Ii", len(samples), data_offset), entries)
        traf = box(b"traf", full(b"tfhd", 0, 0x020000, struct.pack(">I", track_id)),
                   full(b"tfdt", 1, 0, struct.pack(">Q", base_dts)), trun)
        return box(b"moof", full(b"mfhd", 0, 0, struct.pack(">I", seq)), traf)
    moof = build(0)
    moof = build(len(moof) + 8)
    return moof + box(b"mdat", b"".join(x[4] for x in samples))


def video_file():
    stream = header_array("H264D_2", "h264data.h")
    sps, pps, units = video_units(stream)
    w, h = sps_size(sps)
    avcc = bytes([1, sps[1], sps[2], sps[3], 0xFF, 0xE1]) + struct.pack(">H", len(sps)) + sps + \
        bytes([1]) + struct.pack(">H", len(pps)) + pps
    entry = box(b"avc1", b"\0" * 6, struct.pack(">H", 1), b"\0" * 16, struct.pack(">HH", w, h),
                struct.pack(">II", 0x480000, 0x480000), struct.pack(">I", 0), struct.pack(">H", 1), b"\0" * 32,
                struct.pack(">Hh", 0x18, -1), box(b"avcC", avcc))
    timescale, dur = 30000, 1000
    init = init_segment(1, timescale, b"vide", entry, w, h)
    media = b""
    for f in range(0, len(units), 10):
        chunk = units[f:f + 10]
        samples = []
        for i, (idr, idx, au) in enumerate(chunk):
            dts_i = f + i
            # Version 1's offsets may be negative, as encoders write them:
            # presentation starts at 0, not a picture or two later.
            cto = (idx - dts_i) * dur
            flags = 0x02000000 if idr else 0x01010000
            samples.append((dur, len(au), flags, cto, au))
        media += fragment(f // 10 + 1, 1, f * dur, samples)
    return init, media, len(units), w, h


def audio_file():
    ts = header_array("AACD_TS", "aacdata.h")
    # The ADTS frames out of the transport stream (playcheck's demuxer).
    from playcheck import ts_payloads, adts_frames
    frames = adts_frames(b"".join(ts_payloads(ts)[0x0F]))
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
    media = b""
    half = len(raw) // 2
    for n, part in enumerate((raw[:half], raw[half:])):
        samples = [(1024, len(a), 0x02000000, 0, a) for a in part]
        media += fragment(n + 1, 2, n * half * 1024, samples)
    return init, media, len(raw), rate, chans, raw


def c_bytes(name, data):
    o = ["static const unsigned char %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 24):
        o.append("    " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    o.append("};")
    return o


def main():
    os.makedirs(TMP, exist_ok=True)
    vinit, vmedia, frames, w, h = video_file()
    ainit, amedia, aframes, rate, chans, raw = audio_file()
    # Windows reads both as the files they claim to be.
    vfile, afile = os.path.join(TMP, "video.mp4"), os.path.join(TMP, "audio.mp4")
    open(vfile, "wb").write(vinit + vmedia)
    open(afile, "wb").write(ainit + amedia)
    if not os.path.exists(MF):
        import genh264
        genh264.build_mf()
    yuv = os.path.join(TMP, "video.yuv")
    r = subprocess.run([MF, "decode", vfile, yuv], stdout=subprocess.PIPE, text=True)
    if r.returncode or "frames %d" % frames not in r.stdout:
        sys.exit("Windows does not read the fragmented video as %d frames: %s" % (frames, r.stdout.strip()))
    # And decodes them as h264test's answers have them.
    got = open(yuv, "rb").read()
    fs = w * h * 3 // 2
    sums = header_sums(2)

    def fnv(d):
        v = 2166136261
        for b in d:
            v = ((v ^ b) * 16777619) & 0xFFFFFFFF
        return v
    for f in range(frames):
        fr = got[f * fs:(f + 1) * fs]
        if [fnv(fr[:w * h]), fnv(fr[w * h:w * h * 5 // 4]), fnv(fr[w * h * 5 // 4:])] != sums[3 * f:3 * f + 3]:
            sys.exit("Windows' decoding of the fragmented video differs at frame %d" % f)
    print("video: %d frames %dx%d, init %d bytes, media %d bytes; Windows reads it the same" % (frames, w, h, len(vinit), len(vmedia)))
    # The sound too, through Windows' own decoders into a WAV: as long as the
    # frames say, at the rate and channels they say.
    wav = os.path.join(TMP, "audio.wav")
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                    os.path.join(TOOLS, "mftranscode.ps1"), "-In", afile, "-Out", wav,
                    "-Channels", str(chans), "-Rate", str(rate)], check=True)
    d = open(wav, "rb").read()
    at, data = 12, b""
    while at + 8 <= len(d):
        size = struct.unpack("<I", d[at + 4:at + 8])[0]
        if d[at:at + 4] == b"data":
            data = d[at + 8:at + 8 + size]
            break
        at += 8 + size + (size & 1)
    heard = len(data) // (2 * chans)
    if abs(heard - aframes * 1024) > 2048:
        sys.exit("Windows reads the fragmented sound as %d samples, not %d" % (heard, aframes * 1024))
    print("audio: %d frames at %d Hz, %d channels, init %d bytes, media %d bytes; Windows reads %d samples"
          % (aframes, rate, chans, len(ainit), len(amedia), heard))
    o = ["/* Generated by tools/genmedia.py: streams the tests already know, in fragmented",
         "   MP4 as Media Source Extensions append them. Do not edit. */", "#pragma once", ""]
    o += c_bytes("MEDIA_VIDEO_INIT", vinit)
    o += c_bytes("MEDIA_VIDEO_MEDIA", vmedia)
    o.append("#define MEDIA_VIDEO_FRAMES %d" % frames)
    o.append("#define MEDIA_VIDEO_CASE 2          /* its checksums are H264_CASES[2]'s */")
    # The same checksums again, for a test that has no room for h264data.h.
    o.append("static const unsigned MEDIA_VIDEO_SUMS[%d] = {" % len(sums[:3 * frames]))
    for i in range(0, 3 * frames, 6):
        o.append("    " + ", ".join("0x%08x" % v for v in sums[i:i + 6]) + ",")
    o.append("};")
    o += c_bytes("MEDIA_AUDIO_INIT", ainit)
    o += c_bytes("MEDIA_AUDIO_MEDIA", amedia)
    o.append("#define MEDIA_AUDIO_FRAMES %d" % aframes)
    o.append("#define MEDIA_AUDIO_RATE %d" % rate)
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(o) + "\n")
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
