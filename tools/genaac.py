#!/usr/bin/env python3
# The AAC streams userland/aactest.c decodes, and what Windows' own AAC
# decoder makes of them. Writes userland/aacdata.h.
#
#   python tools/genaac.py
#
# The sounds are this project's own, made here sample by sample; the
# encoding is not: Windows' AAC encoder (Media Foundation, through
# tools/mftranscode.ps1) turns each into an M4A, and Windows' decoder turns
# the M4A back into samples. A decoder checked against its own encoder agrees
# with itself and proves nothing, and one checked against its own idea of the
# answer likewise; so the answer is Windows' decoder's, for the same frames.
#
# The frames are read out of the M4A here (its sample table and the
# AudioSpecificConfig in its esds box), so the test needs no container code.
# One stream goes the way Twitch's does as well: its frames put in ADTS
# headers, PES packets and a transport stream here, and that file decoded by
# Windows for the answer, which checks userland/ts.h and the ADTS reader.
# The answer is a stretch of the decoded samples from well inside the sound,
# where the encoder's start-up and end have no part; the test finds where it
# lines up with its own output, since the two decoders need not agree on how
# much of the encoder's start-up delay to leave out.
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "userland", "aacdata.h")
PS = os.path.join(ROOT, "tools", "mftranscode.ps1")
REF_FROM = 12288          # where the stretch of answer starts, in samples
REF_LEN = 1500            # and how long it is


def wav(path, rate, chans, frames):
    data = b"".join(struct.pack("<%dh" % chans, *[max(-32768, min(32767, int(round(v)))) for v in f]) for f in frames)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, chans, rate, rate * chans * 2, chans * 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def read_wav(path):
    raw = open(path, "rb").read()
    i, fmt, data = 12, None, None
    while i + 8 <= len(raw):
        tag, size = raw[i:i + 4], struct.unpack("<I", raw[i + 4:i + 8])[0]
        body = raw[i + 8:i + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif tag == b"data":
            data = body
        i += 8 + size + (size & 1)
    tagfmt, chans, rate, _, _, bits = fmt
    if bits == 16:
        samples = list(struct.unpack("<%dh" % (len(data) // 2), data))
    elif bits == 32 and tagfmt in (3, 0xFFFE):
        samples = [max(-32768, min(32767, int(round(v * 32768)))) for v in struct.unpack("<%df" % (len(data) // 4), data)]
    else:
        sys.exit("a WAV of %d bits, format %d" % (bits, tagfmt))
    return rate, chans, samples


def boxes(buf, start, end):
    i = start
    while i + 8 <= end:
        size, kind = struct.unpack(">I4s", buf[i:i + 8])
        head = 8
        if size == 1:
            size = struct.unpack(">Q", buf[i + 8:i + 16])[0]
            head = 16
        elif size == 0:
            size = end - i
        yield kind.decode("latin-1"), i + head, i + size
        i += size


def find(buf, start, end, path):
    for kind, b, e in boxes(buf, start, end):
        if kind == path[0]:
            return (b, e) if len(path) == 1 else find(buf, b, e, path[1:])
    return None


def descriptor(buf, i):
    tag = buf[i]; i += 1
    n = 0
    for _ in range(4):
        c = buf[i]; i += 1
        n = (n << 7) | (c & 0x7F)
        if not c & 0x80:
            break
    return tag, i, i + n


def m4a(path):
    buf = open(path, "rb").read()
    stbl = find(buf, 0, len(buf), ["moov", "trak", "mdia", "minf", "stbl"])
    b, e = find(buf, stbl[0], stbl[1], ["stsd"])
    entry = b + 8                                        # version, flags, count
    ebox = entry + 8 + 28                                # the mp4a entry's own fields
    eb, ee = find(buf, ebox, entry + struct.unpack(">I", buf[entry:entry + 4])[0], ["esds"])
    i = eb + 4
    tag, i, _ = descriptor(buf, i)                       # ES_Descr
    i += 3
    tag, i, _ = descriptor(buf, i)                       # DecoderConfigDescr
    i += 13
    tag, i, j = descriptor(buf, i)                       # DecSpecificInfo
    if tag != 5:
        sys.exit("no AudioSpecificConfig in " + path)
    asc = buf[i:j]
    b, e = find(buf, stbl[0], stbl[1], ["stsz"])
    size1, count = struct.unpack(">II", buf[b + 4:b + 12])
    sizes = [size1] * count if size1 else list(struct.unpack(">%dI" % count, buf[b + 12:b + 12 + 4 * count]))
    co = find(buf, stbl[0], stbl[1], ["stco"])
    if co:
        n = struct.unpack(">I", buf[co[0] + 4:co[0] + 8])[0]
        offs = list(struct.unpack(">%dI" % n, buf[co[0] + 8:co[0] + 8 + 4 * n]))
    else:
        co = find(buf, stbl[0], stbl[1], ["co64"])
        n = struct.unpack(">I", buf[co[0] + 4:co[0] + 8])[0]
        offs = list(struct.unpack(">%dQ" % n, buf[co[0] + 8:co[0] + 8 + 8 * n]))
    b, e = find(buf, stbl[0], stbl[1], ["stsc"])
    n = struct.unpack(">I", buf[b + 4:b + 8])[0]
    runs = [struct.unpack(">III", buf[b + 8 + 12 * k:b + 20 + 12 * k]) for k in range(n)]
    frames, s = [], 0
    for c in range(len(offs)):
        per = [r[1] for r in runs if r[0] - 1 <= c][-1]
        at = offs[c]
        for _ in range(per):
            if s >= count:
                break
            frames.append(buf[at:at + sizes[s]])
            at += sizes[s]
            s += 1
    return asc, frames


def crc32_mpeg(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def ts_packets(pid, payload, cc, pcr=None):
    """A PES packet or a table as 188-byte packets, the first marked as the start."""
    out, first = [], True
    while payload or first:
        adapt = b""
        if first and pcr is not None:
            base = pcr
            adapt = bytes([0x10]) + struct.pack(">IH", (base >> 1) & 0xFFFFFFFF, ((base & 1) << 15) | 0x7E00)
        room = 184 - (len(adapt) + 1 if adapt else 0)
        chunk = payload[:room]
        payload = payload[room:]
        if len(chunk) < room:                              # stuffing in the adaptation field
            need = room - len(chunk)
            if adapt:
                adapt += b"\xff" * need
            else:
                adapt = (bytes([0x00]) + b"\xff" * (need - 2)) if need >= 2 else b""
                if need == 1:
                    adapt = None
        head = bytes([0x47, (0x40 if first else 0) | (pid >> 8), pid & 0xFF])
        if adapt is None:                                  # one byte of stuffing: an empty adaptation field
            pkt = head + bytes([0x30 | (cc & 15), 0]) + chunk
        elif adapt:
            pkt = head + bytes([0x30 | (cc & 15), len(adapt)]) + adapt + chunk
        else:
            pkt = head + bytes([0x10 | (cc & 15)]) + chunk
        assert len(pkt) == 188, len(pkt)
        out.append(pkt)
        cc += 1
        first = False
    return b"".join(out), cc


def table(pid, body):
    sec = body + struct.pack(">I", crc32_mpeg(body))
    pkt, _ = ts_packets(pid, b"\x00" + sec, 0)
    return pkt


def transport(asc, frames):
    """The frames as Twitch carries them: ADTS in PES in a transport stream."""
    obj = asc[0] >> 3
    sfi = ((asc[0] & 7) << 1) | (asc[1] >> 7)
    chans = (asc[1] >> 3) & 15
    pat = bytes([0x00, 0xB0, 13, 0, 1, 0xC1, 0, 0, 0, 1, 0xF0, 0x00])
    pmt = bytes([0x02, 0xB0, 18, 0, 1, 0xC1, 0, 0, 0xE1, 0x01, 0xF0, 0, 0x0F, 0xE1, 0x01, 0xF0, 0])
    out = table(0, pat) + table(0x1000, pmt)
    cc, pts = 0, 90000
    rate = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000][sfi]
    for k, f in enumerate(frames):
        n = len(f) + 7
        adts = bytes([0xFF, 0xF1, ((obj - 1) << 6) | (sfi << 2) | (chans >> 2),
                      ((chans & 3) << 6) | (n >> 11), (n >> 3) & 0xFF, ((n & 7) << 5) | 0x1F, 0xFC]) + f
        t = pts + k * 1024 * 90000 // rate
        ptsb = bytes([0x21 | ((t >> 29) & 0x0E), (t >> 22) & 0xFF, 0x01 | ((t >> 14) & 0xFE), (t >> 7) & 0xFF, 0x01 | ((t << 1) & 0xFE)])
        body = bytes([0x80, 0x80, 5]) + ptsb + adts
        pes = b"\x00\x00\x01\xc0" + struct.pack(">H", len(body)) + body
        pkt, cc = ts_packets(0x101, pes, cc, pcr=t * 300 // 300)
        out += pkt
        if k % 8 == 7:
            out += table(0, pat) + table(0x1000, pmt)
    return out


def sounds():
    rnd = random.Random(7)
    out = []
    r = 44100
    out.append(("a tone a channel", r, 2, "Medium",
                [(9000 * math.sin(2 * math.pi * 440 * n / r), 7000 * math.sin(2 * math.pi * 660 * n / r)) for n in range(r)]))
    r = 48000
    clicks = []
    for n in range(r):
        v = 600 * math.sin(2 * math.pi * (200 + 3000 * n / (2 * r)) * n / r)
        if n % 4800 < 24:
            v += 20000 * (1 - (n % 4800) / 24.0) * (1 if (n // 4800) % 2 else -1)
        clicks.append((v, -v * 0.7))
    out.append(("clicks over a sweep, for short windows", r, 2, "High", clicks))
    r = 44100
    mono = []
    for n in range(r):
        env = 0.5 + 0.5 * math.sin(2 * math.pi * 3 * n / r)
        mono.append((env * sum(3000 / k * math.sin(2 * math.pi * 180 * k * n / r) for k in range(1, 9)),))
    out.append(("a voice-like sound in one channel", r, 1, "Medium", mono))
    r = 48000
    noise = []
    for n in range(r):
        a = rnd.uniform(-1, 1) * 5000
        b = rnd.uniform(-1, 1) * 1500
        noise.append((a + b, a - b))
    out.append(("noise nearly the same in both channels, for mid/side", r, 2, "Low", noise))
    return out


def run_ps(args):
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", PS] + args
    subprocess.run(cmd, check=True)


def main():
    tmp = tempfile.mkdtemp(prefix="genaac")
    cases = []
    for k, (name, rate, chans, quality, frames) in enumerate(sounds()):
        w, m, back = [os.path.join(tmp, "s%d.%s" % (k, x)) for x in ("wav", "m4a", "back.wav")]
        wav(w, rate, chans, frames)
        run_ps(["-In", w, "-Out", m, "-Quality", quality, "-Channels", str(chans), "-Rate", str(rate)])
        run_ps(["-In", m, "-Out", back, "-Channels", str(chans), "-Rate", str(rate)])
        asc, aframes = m4a(m)
        brate, bchans, ref = read_wav(back)
        if brate != rate or bchans != chans:
            sys.exit("%s came back at %d Hz, %d channels" % (name, brate, bchans))
        cases.append((name, asc, aframes, rate, chans, ref[REF_FROM * chans:(REF_FROM + REF_LEN) * chans]))
        print(name, len(aframes), "frames", len(asc), "byte config")
        if k == 0:
            tsfile = os.path.join(tmp, "s0.ts")
            tsback = os.path.join(tmp, "s0.ts.wav")
            tsdata = transport(asc, aframes)
            open(tsfile, "wb").write(tsdata)
            run_ps(["-In", tsfile, "-Out", tsback, "-Channels", str(chans), "-Rate", str(rate)])
            trate, tchans, tref = read_wav(tsback)
            if trate != rate or tchans != chans:
                sys.exit("the transport stream came back at %d Hz, %d channels" % (trate, tchans))
            ts_case = (tsdata, tref[REF_FROM * chans:(REF_FROM + REF_LEN) * chans])
            print("transport stream", len(tsdata), "bytes")

    o = ["/* Generated by tools/genaac.py: sounds made there, encoded and decoded by Windows'",
         "   own AAC codec (Media Foundation). Do not edit. */",
         "#pragma once", "",
         "typedef struct {",
         "    const char *name;",
         "    const unsigned char *config; int config_len;",
         "    const unsigned char *data; const unsigned short *sizes; int frames;",
         "    int rate, channels;",
         "    const short *answer; int answer_from, answer_len;   /* samples a channel */",
         "} aac_case;", ""]
    for k, (name, asc, frames, rate, chans, ref) in enumerate(cases):
        data = b"".join(frames)
        o.append("static const unsigned char AACD_CONFIG%d[] = { %s };" % (k, ", ".join("0x%02x" % c for c in asc)))
        o.append("static const unsigned short AACD_SIZES%d[%d] = { %s };" % (k, len(frames), ", ".join(str(len(f)) for f in frames)))
        o.append("static const unsigned char AACD_DATA%d[%d] = {" % (k, len(data)))
        for i in range(0, len(data), 40):
            o.append(",".join(str(c) for c in data[i:i + 40]) + ",")
        o.append("};")
        o.append("static const short AACD_ANSWER%d[%d] = {" % (k, len(ref)))
        for i in range(0, len(ref), 24):
            o.append(",".join(str(v) for v in ref[i:i + 24]) + ",")
        o.append("};")
    tsdata, tsref = ts_case
    o.append("/* The first sound as Twitch carries a stream: ADTS frames in PES packets in a")
    o.append("   transport stream, and Windows' decoding of that file. */")
    o.append("static const unsigned char AACD_TS[%d] = {" % len(tsdata))
    for i in range(0, len(tsdata), 40):
        o.append(",".join(str(c) for c in tsdata[i:i + 40]) + ",")
    o.append("};")
    o.append("static const short AACD_TS_ANSWER[%d] = {" % len(tsref))
    for i in range(0, len(tsref), 24):
        o.append(",".join(str(v) for v in tsref[i:i + 24]) + ",")
    o.append("};")
    o.append("#define AACD_TS_FROM %d" % REF_FROM)
    o.append("#define AACD_TS_LEN %d" % REF_LEN)
    o.append("static const aac_case AAC_CASES[%d] = {" % len(cases))
    for k, (name, asc, frames, rate, chans, ref) in enumerate(cases):
        o.append('    { "%s", AACD_CONFIG%d, %d, AACD_DATA%d, AACD_SIZES%d, %d, %d, %d, AACD_ANSWER%d, %d, %d },'
                 % (name, k, len(asc), k, k, len(frames), rate, chans, k, REF_FROM, REF_LEN))
    o.append("};")
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(o) + "\n")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
