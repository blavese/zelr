"""Builds progressive photographs for userland/jpegtest.c, as a C header.

The baseline photographs in jpegdata.h are encoded by the drawing library
Windows has, and it writes only baseline files. Nothing on the host writes a
progressive one, so this is an encoder: small, slow, written from the
specification (ITU T.81, Annex G for the progressive scans and Annex K for
the Huffman tables) and sharing nothing with userland/jpeg.h, which is what
it exists to check.

Every picture is written twice from the same quantised coefficients: once as
a baseline file and once as a progressive one. A progressive file is only a
different order of sending those coefficients -- the low frequencies of every
block first, then the rest, then the low bits the first passes left out -- so
a decoder has to make exactly the same pixels from both, byte for byte. That
is a check with no tolerance in it, which a lossy format otherwise never
allows.

The scans are the ones libjpeg's progressive mode sends: the DC of every
component together with its last bit held back, the brightness's first five
frequencies and then the rest with two bits held back, the colour
differences whole with one held back, and then the refinements that send the
held-back bits. That is every kind of progressive scan there is: DC first and
refined, AC by spectral selection, AC refined by successive approximation,
and runs of empty blocks.

  python tools/genjpegprog.py
"""
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "userland", "jpegprog.h")

# The sixty four frequencies in the order they are sent: zigzag index to
# position in the block.
ZIGZAG = [
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
]

# Annex K's example tables, in block order, scaled for a middling quality.
LUMA_Q = [
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
]
CHROMA_Q = [
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
] + [99] * 32


def scaled(table, quality=75):
    s = 200 - 2 * quality
    return [max(1, min(255, (t * s + 50) // 100)) for t in table]


# --- the pictures -------------------------------------------------------------

def quarters(x, y, w, h):
    if x < w // 2:
        return (255, 0, 0) if y < h // 2 else (0, 0, 255)
    return (0, 255, 0) if y < h // 2 else (255, 255, 255)


def gradient(x, y, w, h):
    v = x * 255 // w
    return (v, v, v)


def rings(x, y, w, h):
    """Detail everywhere, so every frequency has something in it."""
    d = (x - w / 2.0) ** 2 + (y - h / 2.0) ** 2
    v = int(127 + 120 * math.cos(d / 18.0))
    return (v, (x * 7) % 256, (y * 11) % 256)


# name, width, height, pixels, sampling (h, v of brightness), grey, restart
PICTURES = [
    ("quarters", 64, 64, quarters, (2, 2), False, 0),
    ("tall", 21, 37, quarters, (2, 2), False, 0),
    ("ramp", 48, 24, gradient, (1, 1), False, 0),
    ("rings", 40, 40, rings, (2, 1), False, 0),
    ("grey", 30, 20, rings, (1, 1), True, 0),
    ("restarts", 40, 40, rings, (2, 2), False, 2),
]


# --- colour, blocks and the forward transform -------------------------------------

COS = [[math.cos((2 * x + 1) * u * math.pi / 16.0) for x in range(8)] for u in range(8)]


def fdct(samples):
    """Sixty four samples, level shifted, to sixty four frequencies, in block
    order: the formula, rows and then columns."""
    tmp = [0.0] * 64
    for y in range(8):
        for u in range(8):
            cu = math.sqrt(0.5) if u == 0 else 1.0
            tmp[y * 8 + u] = 0.5 * cu * sum(samples[y * 8 + x] * COS[u][x] for x in range(8))
    out = [0.0] * 64
    for u in range(8):
        for v in range(8):
            cv = math.sqrt(0.5) if v == 0 else 1.0
            out[v * 8 + u] = 0.5 * cv * sum(tmp[y * 8 + u] * COS[v][y] for y in range(8))
    return out


def planes(w, h, pixel, samp, grey):
    """The components as (h, v, table, plane width, plane height, samples),
    each plane padded out to whole MCUs by repeating the edge."""
    hmax, vmax = samp
    comps = [(hmax, vmax, 0)] if grey else [(hmax, vmax, 0), (1, 1, 1), (1, 1, 1)]
    mcux = -(-w // (8 * hmax))
    mcuy = -(-h // (8 * vmax))
    rgb = [[pixel(min(x, w - 1), min(y, h - 1), w, h) for x in range(w)] for y in range(h)]

    out = []
    for ci, (hs, vs, tq) in enumerate(comps):
        pw, ph = mcux * hs * 8, mcuy * vs * 8
        sx, sy = hmax // hs, vmax // vs
        plane = [0.0] * (pw * ph)
        for py in range(ph):
            for px in range(pw):
                acc = 0.0
                for dy in range(sy):
                    for dx in range(sx):
                        x = min(px * sx + dx, w - 1)
                        y = min(py * sy + dy, h - 1)
                        r, g, b = rgb[y][x]
                        if ci == 0:
                            acc += 0.299 * r + 0.587 * g + 0.114 * b
                        elif ci == 1:
                            acc += -0.168736 * r - 0.331264 * g + 0.5 * b + 128
                        else:
                            acc += 0.5 * r - 0.418688 * g - 0.081312 * b + 128
                plane[py * pw + px] = acc / (sx * sy)
        out.append((hs, vs, tq, pw, ph, plane))
    return out, mcux, mcuy


def coefficients(comps, tables):
    """Every block of every component, quantised: [comp][by][bx] -> 64 ints in
    block order."""
    out = []
    for hs, vs, tq, pw, ph, plane in comps:
        q = tables[tq]
        rows = []
        for by in range(ph // 8):
            row = []
            for bx in range(pw // 8):
                samples = [plane[(by * 8 + y) * pw + bx * 8 + x] - 128.0
                           for y in range(8) for x in range(8)]
                f = fdct(samples)
                row.append([int(round(f[i] / q[i])) for i in range(64)])
            rows.append(row)
        out.append(rows)
    return out


# --- Huffman tables (Annex K.2) --------------------------------------------------

def optimal_table(freq):
    """Code lengths for the symbols that occur, no code longer than sixteen
    and none all ones, as the (bits, values) a DHT carries."""
    f = [0] * 257
    for s, n in freq.items():
        f[s] = n
    f[256] = 1                                  # the reserved code point
    size = [0] * 257
    others = [-1] * 257
    while True:
        c1 = -1
        v = None
        for i in range(257):
            if f[i] and (v is None or f[i] <= v):
                v, c1 = f[i], i
        c2 = -1
        v = None
        for i in range(257):
            if f[i] and i != c1 and (v is None or f[i] <= v):
                v, c2 = f[i], i
        if c2 < 0:
            break
        f[c1] += f[c2]
        f[c2] = 0
        size[c1] += 1
        while others[c1] >= 0:
            c1 = others[c1]
            size[c1] += 1
        others[c1] = c2
        size[c2] += 1
        while others[c2] >= 0:
            c2 = others[c2]
            size[c2] += 1
    bits = [0] * 33
    for i in range(257):
        if size[i]:
            bits[size[i]] += 1
    for i in range(32, 16, -1):
        while bits[i] > 0:
            j = i - 2
            while bits[j] == 0:
                j -= 1
            bits[i] -= 2
            bits[i - 1] += 1
            bits[j + 1] += 2
            bits[j] -= 1
    i = 16
    while bits[i] == 0:
        i -= 1
    bits[i] -= 1                                # the reserved one goes
    values = [s for n in range(1, 33) for s in range(256) if size[s] == n]
    return bits[1:17], values


def codes_of(bits, values):
    code, k, out = 0, 0, {}
    for n in range(1, 17):
        for _ in range(bits[n - 1]):
            out[values[k]] = (code, n)
            code += 1
            k += 1
        code <<= 1
    return out


# --- writing bits --------------------------------------------------------------------

class Counter:
    """The first pass over a scan: which symbols each table is asked for."""
    def __init__(self):
        self.freq = {}

    def symbol(self, table, s):
        self.freq.setdefault(table, {})
        self.freq[table][s] = self.freq[table].get(s, 0) + 1

    def bits(self, value, n):
        pass

    def restart(self, n):
        pass


class Writer:
    """The second: the symbols as codes and the bits after them, a 0xFF
    followed by a 0x00, and a restart marker on a byte boundary."""
    def __init__(self, codes):
        self.codes = codes
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, value, n):
        for i in range(n - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                if self.acc == 0xFF:
                    self.out.append(0)
                self.acc = 0
                self.n = 0

    def symbol(self, table, s):
        code, n = self.codes[table][s]
        self.put(code, n)

    def bits(self, value, n):
        if n:
            self.put(value & ((1 << n) - 1), n)

    def pad(self):
        if self.n:
            self.put((1 << (8 - self.n)) - 1, 8 - self.n)

    def restart(self, n):
        self.pad()
        self.out += bytes([0xFF, 0xD0 + (n & 7)])


def nbits(v):
    return v.bit_length()


def magnitude_bits(v):
    """A value as the size category and the bits the format writes after it:
    the value itself if positive, one less than it if negative."""
    n = nbits(abs(v))
    return n, (v if v >= 0 else (v - 1)) & ((1 << n) - 1) if n else 0


# --- the scans -------------------------------------------------------------------------

def blocks_of(scan_comps, comps, w, h, hmax, vmax, mcux, mcuy):
    """The blocks a scan sends, in its order, grouped into MCUs: every block
    of every component MCU by MCU for a scan of several, and for a scan of one
    only the blocks that cover the picture, row by row, one to an MCU."""
    if len(scan_comps) > 1:
        for my in range(mcuy):
            for mx in range(mcux):
                mcu = []
                for c in scan_comps:
                    hs, vs = comps[c][0], comps[c][1]
                    for by in range(vs):
                        for bx in range(hs):
                            mcu.append((c, my * vs + by, mx * hs + bx))
                yield mcu
    else:
        c = scan_comps[0]
        hs, vs = comps[c][0], comps[c][1]
        bw = -(-(-(-w * hs // hmax)) // 8)
        bh = -(-(-(-h * vs // vmax)) // 8)
        for by in range(bh):
            for bx in range(bw):
                yield [(c, by, bx)]


def dc_table(c):
    return ("dc", 0 if c == 0 else 1)


def ac_table(c):
    return ("ac", 0 if c == 0 else 1)


def run_scan(sink, kind, scan_comps, ss, se, ah, al, coefs, mcus, restart):
    """One scan, into a Counter or a Writer."""
    pred = {}
    state = {"eobrun": 0, "be": []}
    table = ac_table(scan_comps[0])

    def emit_eobrun():
        if state["eobrun"]:
            n = nbits(state["eobrun"]) - 1
            sink.symbol(table, n << 4)
            if n:
                sink.bits(state["eobrun"], n)
            state["eobrun"] = 0
            for b in state["be"]:
                sink.bits(b, 1)
            state["be"] = []

    count = 0
    for i, mcu in enumerate(mcus):
        if restart and i and i % restart == 0:
            emit_eobrun()
            sink.restart(count)
            count += 1
            pred = {}
        for c, by, bx in mcu:
            blk = coefs[c][by][bx]
            if kind == "baseline":
                d = blk[0] - pred.get(c, 0)
                pred[c] = blk[0]
                n, v = magnitude_bits(d)
                sink.symbol(dc_table(c), n)
                sink.bits(v, n)
                r = 0
                for k in range(1, 64):
                    t = blk[ZIGZAG[k]]
                    if t == 0:
                        r += 1
                        continue
                    while r > 15:
                        sink.symbol(ac_table(c), 0xF0)
                        r -= 16
                    n, v = magnitude_bits(t)
                    sink.symbol(ac_table(c), (r << 4) | n)
                    sink.bits(v, n)
                    r = 0
                if r:
                    sink.symbol(ac_table(c), 0x00)
            elif kind == "dc first":
                t = blk[0] >> al                    # arithmetic, as the point transform is
                d = t - pred.get(c, 0)
                pred[c] = t
                n, v = magnitude_bits(d)
                sink.symbol(dc_table(c), n)
                sink.bits(v, n)
            elif kind == "dc refine":
                sink.bits((blk[0] >> al) & 1, 1)
            elif kind == "ac first":
                r = 0
                for k in range(ss, se + 1):
                    t = blk[ZIGZAG[k]]
                    t = -((-t) >> al) if t < 0 else t >> al    # towards zero
                    if t == 0:
                        r += 1
                        continue
                    emit_eobrun()
                    while r > 15:
                        sink.symbol(table, 0xF0)
                        r -= 16
                    n, v = magnitude_bits(t)
                    sink.symbol(table, (r << 4) | n)
                    sink.bits(v, n)
                    r = 0
                if r:
                    state["eobrun"] += 1
                    if state["eobrun"] == 0x7FFF:
                        emit_eobrun()
            else:                                   # ac refine
                absv = {k: abs(blk[ZIGZAG[k]]) >> al for k in range(ss, se + 1)}
                eob = max([k for k in absv if absv[k] == 1], default=-1)
                r = 0
                br = []
                for k in range(ss, se + 1):
                    t = absv[k]
                    if t == 0:
                        r += 1
                        continue
                    while r > 15 and k <= eob:
                        emit_eobrun()
                        sink.symbol(table, 0xF0)
                        r -= 16
                        for b in br:
                            sink.bits(b, 1)
                        br = []
                    if t > 1:
                        br.append(t & 1)              # a correction to one already sent
                        continue
                    emit_eobrun()
                    sink.symbol(table, (r << 4) | 1)
                    sink.bits(0 if blk[ZIGZAG[k]] < 0 else 1, 1)
                    for b in br:
                        sink.bits(b, 1)
                    br = []
                    r = 0
                if r or br:
                    state["eobrun"] += 1
                    state["be"] += br
                    if state["eobrun"] == 0x7FFF or len(state["be"]) > 1000 - 64 + 1:
                        emit_eobrun()
    emit_eobrun()


def segment(marker, body):
    return bytes([0xFF, marker]) + (len(body) + 2).to_bytes(2, "big") + body


def dht(tables, codes_out):
    body = bytearray()
    for (cls, idx), (bits, values) in tables:
        body.append(((1 if cls == "ac" else 0) << 4) | idx)
        body += bytes(bits) + bytes(values)
        codes_out[(cls, idx)] = codes_of(bits, values)
    return segment(0xC4, bytes(body))


def encode(w, h, pixel, samp, grey, restart):
    """The same coefficients as a baseline file and as a progressive one."""
    hmax, vmax = samp
    qt = [scaled(LUMA_Q), scaled(CHROMA_Q)]
    comps, mcux, mcuy = planes(w, h, pixel, samp, grey)
    coefs = coefficients(comps, qt)
    ncomp = len(comps)

    def head(sof):
        out = bytearray(b"\xFF\xD8")
        for i in range(1 if grey else 2):
            out += segment(0xDB, bytes([i]) + bytes(qt[i][ZIGZAG[k]] for k in range(64)))
        body = bytearray([8]) + h.to_bytes(2, "big") + w.to_bytes(2, "big") + bytes([ncomp])
        for i, (hs, vs, tq, _, _, _) in enumerate(comps):
            body += bytes([i + 1, (hs << 4) | vs, tq])
        out += segment(sof, bytes(body))
        if restart:
            out += segment(0xDD, restart.to_bytes(2, "big"))
        return out

    def scan(out, kind, scan_comps, ss, se, ah, al):
        mcus = list(blocks_of(scan_comps, comps, w, h, hmax, vmax, mcux, mcuy))
        counter = Counter()
        run_scan(counter, kind, scan_comps, ss, se, ah, al, coefs, mcus, restart)
        codes = {}
        if counter.freq:
            out += dht([(t, optimal_table(f)) for t, f in sorted(counter.freq.items())], codes)
        body = bytearray([len(scan_comps)])
        for c in scan_comps:
            body += bytes([c + 1, ((0 if c == 0 else 1) << 4) | (0 if c == 0 else 1)])
        body += bytes([ss, se, (ah << 4) | al])
        out += segment(0xDA, bytes(body))
        writer = Writer(codes)
        run_scan(writer, kind, scan_comps, ss, se, ah, al, coefs, mcus, restart)
        writer.pad()
        out += writer.out

    base = head(0xC0)
    scan(base, "baseline", list(range(ncomp)), 0, 63, 0, 0)
    base += b"\xFF\xD9"

    prog = head(0xC2)
    all_c = list(range(ncomp))
    if grey:
        script = [("dc first", [0], 0, 0, 0, 1), ("ac first", [0], 1, 5, 0, 2),
                  ("ac first", [0], 6, 63, 0, 2), ("ac refine", [0], 1, 63, 2, 1),
                  ("dc refine", [0], 0, 0, 1, 0), ("ac refine", [0], 1, 63, 1, 0)]
    else:
        script = [("dc first", all_c, 0, 0, 0, 1), ("ac first", [0], 1, 5, 0, 2),
                  ("ac first", [2], 1, 63, 0, 1), ("ac first", [1], 1, 63, 0, 1),
                  ("ac first", [0], 6, 63, 0, 2), ("ac refine", [0], 1, 63, 2, 1),
                  ("dc refine", all_c, 0, 0, 1, 0), ("ac refine", [2], 1, 63, 1, 0),
                  ("ac refine", [1], 1, 63, 1, 0), ("ac refine", [0], 1, 63, 1, 0)]
    for kind, sc, ss, se, ah, al in script:
        scan(prog, kind, sc, ss, se, ah, al)
    prog += b"\xFF\xD9"
    return bytes(base), bytes(prog)


def carray(name, data):
    out = "static const u8 %s[%d] = {\n" % (name, len(data))
    for i in range(0, len(data), 16):
        out += "    " + ", ".join(str(b) for b in data[i:i + 16]) + ",\n"
    return out + "};\n\n"


def main():
    text = ['/* Generated by tools/genjpegprog.py. Do not edit by hand.\n'
            '\n'
            '   Each picture twice from the same quantised coefficients, by an\n'
            '   encoder that shares nothing with userland/jpeg.h: a baseline file\n'
            '   (JPB_) and a progressive one (JPP_), which have to decode to the\n'
            '   same pixels exactly. See that tool for which scans are in them. */\n'
            '#pragma once\n'
            '#include "zelr.h"\n\n']
    for name, w, h, pixel, samp, grey, restart in PICTURES:
        base, prog = encode(w, h, pixel, samp, grey, restart)
        up = name.upper()
        text.append("/* %d by %d%s%s */\n" % (w, h, ", grey" if grey else "",
                                              ", restarts every %d" % restart if restart else ""))
        text.append("#define JP_%s_W %d\n#define JP_%s_H %d\n" % (up, w, up, h))
        text.append(carray("JPB_" + up, base))
        text.append(carray("JPP_" + up, prog))
        print("  %-9s %d by %d, baseline %d bytes, progressive %d bytes"
              % (name, w, h, len(base), len(prog)))
    f = open(OUT, "w", newline="\n")
    f.write("".join(text))
    f.close()
    print("wrote %s" % OUT)


if __name__ == "__main__":
    main()
