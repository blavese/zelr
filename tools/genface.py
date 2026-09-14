#!/usr/bin/env python3
"""Draws the interface typeface and rasterises it with anti-aliasing.

The old font is 95 glyphs of hand-drawn text art on an 8x16 grid, one bit per
pixel, at exactly one size. It is legible and it is the single thing doing the
most damage to how this system looks: every character on screen has hard
stairstep edges, and no amount of rounded corners or shadows anywhere else
compensates for that. You can polish the chrome forever and it will still read
as 1995 while the text is a bitmap.

So this is a different thing. Each glyph is an outline on a 1000 unit em, the
way a real typeface is described, and the outlines are filled by measuring how
much of each pixel lands inside them. That produces an eight bit coverage
value per pixel instead of a yes or no, and it works at any size, because the
outline is scaled before it is measured rather than after.

Nothing is imported and nothing is traced. The letterforms are built here out
of stems, bars and arcs, with the proportions written down once at the top so
the whole face stays consistent rather than each glyph being drawn to taste.

  python tools/genface.py            write the C and the header
  python tools/genface.py --preview  print a size as text, to look at it
"""
import sys

EM = 1000

# --- proportions ----------------------------------------------------------
#
# Written down once. Every glyph below is built from these rather than from
# numbers chosen per letter, which is what keeps a face looking like one face.

BASELINE = 0            # y grows upward here; the rasteriser flips it
CAP = 700               # flat capitals reach this
XH = 500                # lowercase without ascenders
ASC = 730               # b d f h k l
DESC = -210             # g j p q y
STEM = 92               # a vertical stroke
THIN = 78               # a horizontal one, lighter, as the eye expects
OVER = 12               # round shapes overshoot flat ones or they look small

ADV = 560               # default advance
SIDE = 70               # space either side of a glyph


def _bez(p0, p1, p2, steps=12):
    """A quadratic curve, flattened. Twelve steps is past what is visible at
    the sizes this is rasterised to."""
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1 - t
        out.append((u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
                    u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]))
    return out


class Pen:
    """Builds closed contours. Everything a glyph is made of ends up here as
    a list of points."""

    def __init__(self):
        self.contours = []
        self._cur = None

    def move(self, x, y):
        self._flush()
        self._cur = [(x, y)]
        return self

    def line(self, x, y):
        self._cur.append((x, y))
        return self

    def curve(self, cx, cy, x, y):
        self._cur.extend(_bez(self._cur[-1], (cx, cy), (x, y)))
        return self

    def close(self):
        self._flush()
        return self

    def _flush(self):
        if self._cur and len(self._cur) > 2:
            self.contours.append(self._cur)
        self._cur = None

    # --- the primitives every letter is made of --------------------------

    def stem(self, x, y0, y1, w=STEM):
        """A vertical stroke, centred on x."""
        return self.move(x - w / 2, y0).line(x + w / 2, y0) \
                   .line(x + w / 2, y1).line(x - w / 2, y1).close()

    def bar(self, x0, x1, y, h=THIN):
        """A horizontal stroke, centred on y."""
        return self.move(x0, y - h / 2).line(x1, y - h / 2) \
                   .line(x1, y + h / 2).line(x0, y + h / 2).close()

    def ring(self, cx, cy, rx, ry, w, a0=0, a1=360, steps=48):
        """An arc of given thickness: the outer edge one way, the inner edge
        back. Bowls, shoulders and the round letters are all this."""
        import math
        a0r, a1r = math.radians(a0), math.radians(a1)
        span = a1r - a0r
        pts = []
        for i in range(steps + 1):
            a = a0r + span * i / steps
            pts.append((cx + math.cos(a) * (rx + w / 2),
                        cy + math.sin(a) * (ry + w / 2)))
        for i in range(steps, -1, -1):
            a = a0r + span * i / steps
            pts.append((cx + math.cos(a) * (rx - w / 2),
                        cy + math.sin(a) * (ry - w / 2)))
        self._flush()
        self.contours.append(pts)
        return self

    def diag(self, x0, y0, x1, y1, w=STEM):
        """A sloped stroke. The width is measured across the stroke, not
        horizontally, or the diagonals of A and V come out heavier than the
        stems they meet."""
        import math
        dx, dy = x1 - x0, y1 - y0
        n = math.hypot(dx, dy) or 1
        ox, oy = -dy / n * w / 2, dx / n * w / 2
        return self.move(x0 + ox, y0 + oy).line(x1 + ox, y1 + oy) \
                   .line(x1 - ox, y1 - oy).line(x0 - ox, y0 - oy).close()

    def dot(self, cx, cy, r):
        import math
        pts = [(cx + math.cos(math.radians(a * 12)) * r,
                cy + math.sin(math.radians(a * 12)) * r) for a in range(30)]
        self._flush()
        self.contours.append(pts)
        return self


# --- rasterising ----------------------------------------------------------
#
# How much of each pixel falls inside the outline, as a number from 0 to 255.
# Sampled on a grid four times finer than the pixels in both directions, so a
# pixel is decided by sixteen samples rather than by whether its centre
# happens to land inside a stroke. That sixteenth of a pixel is the difference
# between a letter with smooth edges and one with stairs.

SUB = 4


def _coverage(edges, w, h, into):
    """Fills one contour's coverage into `into`, taking the larger value where
    it already has one."""
    for py in range(h):
        row = into[py]
        for sy in range(SUB):
            y = py + (sy + 0.5) / SUB
            hits = []
            for ax, ay, bx, by in edges:
                if (ay <= y < by) or (by <= y < ay):
                    hits.append(ax + (y - ay) * (bx - ax) / (by - ay))
            if len(hits) < 2:
                continue
            hits.sort()
            for i in range(0, len(hits) - 1, 2):
                sx, ex = hits[i], hits[i + 1]
                if ex <= 0 or sx >= w:
                    continue
                first, last = int(max(sx, 0)), int(min(ex, w - 0.0001))
                for px in range(first, last + 1):
                    covered = min(ex, px + 1) - max(sx, px)
                    if covered > 0:
                        row[px] += covered * (255 / SUB)


def rasterise(contours, px_size, pad=1):
    """Returns (width, height, left, top, rows) with rows as bytearrays of
    coverage. `top` is how far above the baseline the bitmap starts.

    Each contour is measured on its own and the results are combined by
    taking the larger value, which is a union. Measuring them all together
    with one even-odd pass is the obvious thing and it is wrong: a letter
    here is built from overlapping strokes, and where the crossbar of an H
    crosses its stems the two overlaps cancel and the stems disappear from
    those rows. That is what the first version of this did, and it is
    visible immediately in a preview and not at all in a passing test.
    """
    if not contours:
        return 0, 0, 0, 0, []

    scale = px_size / EM
    xs = [p[0] for c in contours for p in c]
    ys = [p[1] for c in contours for p in c]

    x0 = int(min(xs) * scale) - pad
    x1 = int(max(xs) * scale) + pad + 1
    y0 = int(min(ys) * scale) - pad
    y1 = int(max(ys) * scale) + pad + 1

    w, h = x1 - x0, y1 - y0
    if w <= 0 or h <= 0:
        return 0, 0, 0, 0, []

    acc = [[0.0] * w for _ in range(h)]

    for c in contours:
        edges = []
        n = len(c)
        for i in range(n):
            ax, ay = c[i]
            bx, by = c[(i + 1) % n]
            ax, ay = ax * scale - x0, (y1 - ay * scale)
            bx, by = bx * scale - x0, (y1 - by * scale)
            if ay != by:
                edges.append((ax, ay, bx, by))
        if not edges:
            continue
        one = [[0.0] * w for _ in range(h)]
        _coverage(edges, w, h, one)
        for py in range(h):
            a, o = acc[py], one[py]
            for px in range(w):
                if o[px] > a[px]:
                    a[px] = o[px]

    rows = []
    for py in range(h):
        rows.append(bytearray(255 if v > 255 else int(v) for v in acc[py]))
    return w, h, x0, y1, rows


def preview(contours, px_size):
    """As text art, because a number cannot be looked at."""
    w, h, left, top, rows = rasterise(contours, px_size)
    shades = " .:-=+*#%@"
    out = []
    for r in rows:
        out.append("".join(shades[min(9, v * 10 // 256)] for v in r))
    return out, w, h, left, top


# --- the letterforms -------------------------------------------------------
#
# Each entry is a function returning (Pen, advance). They are built from the
# primitives above and from the proportions at the top of this file, so the
# face is consistent by construction: changing STEM changes every stroke, and
# changing XH moves every lowercase letter together.

def _bowl(p, x0, x1, y0, y1, w=STEM, a0=0, a1=360):
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    return p.ring(cx, cy, (x1 - x0) / 2.0 - w / 2, (y1 - y0) / 2.0 - w / 2, w, a0, a1)


L = SIDE                       # left edge of most glyphs
R = ADV - SIDE                 # right edge
MID = ADV / 2.0

G = {}


def glyph(ch, adv=None):
    def wrap(fn):
        G[ch] = (fn, adv if adv is not None else ADV)
        return fn
    return wrap


# --- space and punctuation ------------------------------------------------

@glyph(" ", 300)
def _sp(p): return p

@glyph("!", 300)
def _bang(p):
    p.stem(150, 170, CAP, STEM * 0.95)
    p.dot(150, 55, STEM * 0.58)
    return p

@glyph('"', 360)
def _dq(p):
    p.stem(115, CAP - 190, CAP, STEM * 0.8)
    p.stem(245, CAP - 190, CAP, STEM * 0.8)
    return p

@glyph("#", 620)
def _hash(p):
    p.diag(160, 0, 230, CAP, THIN * 0.85)
    p.diag(370, 0, 440, CAP, THIN * 0.85)
    p.bar(60, 540, CAP * 0.66, THIN * 0.85)
    p.bar(60, 540, CAP * 0.30, THIN * 0.85)
    return p

@glyph("$", 560)
def _dollar(p):
    _bowl(p, L, R, CAP * 0.46, CAP * 0.96, STEM * 0.9, 20, 250)
    _bowl(p, L, R, CAP * 0.04, CAP * 0.54, STEM * 0.9, 200, 430)
    p.stem(MID, -70, CAP + 70, STEM * 0.7)
    return p

@glyph("%", 720)
def _pct(p):
    p.ring(170, CAP * 0.76, 100, CAP * 0.16, STEM * 0.8)
    p.ring(545, CAP * 0.22, 100, CAP * 0.16, STEM * 0.8)
    p.diag(560, CAP, 150, 0, STEM * 0.8)
    return p

@glyph("&", 660)
def _amp(p):
    _bowl(p, 110, 430, CAP * 0.56, CAP, STEM * 0.9)
    _bowl(p, 70, 470, 0, CAP * 0.52, STEM * 0.9, 20, 340)
    p.diag(250, CAP * 0.56, 590, 0, STEM * 0.9)
    return p

@glyph("'", 240)
def _sq(p):
    p.stem(120, CAP - 190, CAP, STEM * 0.8)
    return p

@glyph("(", 320)
def _lp(p):
    p.ring(300, CAP * 0.45, 230, CAP * 0.62, STEM * 0.82, 130, 230)
    return p

@glyph(")", 320)
def _rp(p):
    p.ring(20, CAP * 0.45, 230, CAP * 0.62, STEM * 0.82, -50, 50)
    return p

@glyph("*", 420)
def _star(p):
    for a in (90, 150, 210, 270, 330, 30):
        import math
        r = 150
        p.diag(210, CAP * 0.78,
               210 + math.cos(math.radians(a)) * r,
               CAP * 0.78 + math.sin(math.radians(a)) * r, THIN * 0.8)
    return p

@glyph("+", 560)
def _plus(p):
    p.bar(100, 460, CAP * 0.44, THIN)
    p.stem(280, CAP * 0.44 - 180, CAP * 0.44 + 180, THIN)
    return p

@glyph(",", 260)
def _comma(p):
    p.stem(120, -120, 90, STEM * 0.85)
    p.diag(120, -40, 60, -150, STEM * 0.8)
    return p

@glyph("-", 400)
def _minus(p):
    p.bar(70, 330, XH * 0.52, THIN)
    return p

@glyph(".", 260)
def _dot(p):
    p.dot(120, 55, STEM * 0.58)
    return p

@glyph("/", 440)
def _slash(p):
    p.diag(40, -60, 400, CAP + 60, STEM * 0.85)
    return p

# --- digits ---------------------------------------------------------------

@glyph("0")
def _0(p):
    _bowl(p, L, R, 0, CAP)
    return p

@glyph("1")
def _1(p):
    p.stem(300, 0, CAP)
    p.diag(150, CAP * 0.80, 300, CAP, STEM * 0.9)
    p.bar(130, 470, THIN / 2)
    return p

@glyph("2")
def _2(p):
    _bowl(p, L, R, CAP * 0.46, CAP, STEM, 0, 200)
    p.diag(R - 20, CAP * 0.70, 110, THIN, STEM)
    p.bar(L, R, THIN / 2)
    return p

@glyph("3")
def _3(p):
    _bowl(p, 100, R, CAP * 0.50, CAP, STEM, -80, 190)
    _bowl(p, 100, R, 0, CAP * 0.50, STEM, 170, 440)
    return p

@glyph("4")
def _4(p):
    p.stem(R - 90, 0, CAP)
    p.diag(R - 90, CAP, 60, CAP * 0.26, STEM * 0.9)
    p.bar(50, R, CAP * 0.26)
    return p

@glyph("5")
def _5(p):
    p.bar(L, R - 20, CAP - THIN / 2)
    p.stem(L + STEM / 2, CAP * 0.52, CAP)
    p.bar(L, MID + 60, CAP * 0.52)
    _bowl(p, 90, R, 0, CAP * 0.56, STEM, -110, 150)
    return p

@glyph("6")
def _6(p):
    _bowl(p, L, R, 0, CAP * 0.56)
    p.ring(MID, CAP * 0.44, (R - L) / 2 - STEM / 2, CAP * 0.42, STEM, 95, 175)
    return p

@glyph("7")
def _7(p):
    p.bar(L, R, CAP - THIN / 2)
    p.diag(R - 40, CAP, 170, 0, STEM)
    return p

@glyph("8")
def _8(p):
    _bowl(p, 90, R - 20, CAP * 0.52, CAP)
    _bowl(p, L, R, 0, CAP * 0.54)
    return p

@glyph("9")
def _9(p):
    _bowl(p, L, R, CAP * 0.44, CAP)
    p.ring(MID, CAP * 0.56, (R - L) / 2 - STEM / 2, CAP * 0.42, STEM, -85, -5)
    return p

# --- more punctuation -----------------------------------------------------

@glyph(":", 260)
def _colon(p):
    p.dot(120, 55, STEM * 0.58)
    p.dot(120, XH * 0.72, STEM * 0.58)
    return p

@glyph(";", 260)
def _semi(p):
    p.dot(120, XH * 0.72, STEM * 0.58)
    p.stem(120, -120, 90, STEM * 0.85)
    p.diag(120, -40, 60, -150, STEM * 0.8)
    return p

@glyph("<", 520)
def _lt(p):
    p.diag(430, CAP * 0.78, 90, CAP * 0.40, THIN)
    p.diag(430, CAP * 0.02, 90, CAP * 0.40, THIN)
    return p

@glyph("=", 560)
def _eq(p):
    p.bar(90, 470, CAP * 0.56, THIN)
    p.bar(90, 470, CAP * 0.28, THIN)
    return p

@glyph(">", 520)
def _gt(p):
    p.diag(90, CAP * 0.78, 430, CAP * 0.40, THIN)
    p.diag(90, CAP * 0.02, 430, CAP * 0.40, THIN)
    return p

@glyph("?", 500)
def _q(p):
    _bowl(p, 60, 440, CAP * 0.52, CAP, STEM, 0, 200)
    p.stem(250, CAP * 0.24, CAP * 0.62, STEM)
    p.dot(250, 55, STEM * 0.58)
    return p

@glyph("@", 780)
def _at(p):
    _bowl(p, 40, 740, 0, CAP, STEM * 0.85, 0, 330)
    _bowl(p, 250, 530, CAP * 0.26, CAP * 0.66, STEM * 0.8)
    p.stem(530 - STEM / 2, CAP * 0.26, CAP * 0.52, STEM * 0.8)
    return p

# --- capitals -------------------------------------------------------------

@glyph("A", 640)
def _A(p):
    p.diag(40, 0, 320, CAP)
    p.diag(600, 0, 320, CAP)
    p.bar(130, 510, CAP * 0.30)
    return p

@glyph("B", 600)
def _B(p):
    p.stem(L + STEM / 2, 0, CAP)
    p.bar(L, 420, CAP - THIN / 2)
    p.bar(L, 440, CAP * 0.52)
    p.bar(L, 430, THIN / 2)
    p.ring(420, CAP * 0.76, 110, CAP * 0.24 - STEM / 4, STEM, -90, 90)
    p.ring(430, CAP * 0.26, 120, CAP * 0.26 - STEM / 4, STEM, -90, 90)
    return p

@glyph("C", 620)
def _C(p):
    _bowl(p, L, 560, 0, CAP, STEM, 38, 322)
    return p

@glyph("D", 630)
def _D(p):
    p.stem(L + STEM / 2, 0, CAP)
    p.bar(L, 400, CAP - THIN / 2)
    p.bar(L, 400, THIN / 2)
    p.ring(400, CAP / 2, 150, CAP / 2 - STEM / 4, STEM, -90, 90)
    return p

@glyph("E", 560)
def _E(p):
    p.stem(L + STEM / 2, 0, CAP)
    p.bar(L, 490, CAP - THIN / 2)
    p.bar(L, 450, CAP * 0.50)
    p.bar(L, 490, THIN / 2)
    return p

@glyph("F", 540)
def _F(p):
    p.stem(L + STEM / 2, 0, CAP)
    p.bar(L, 480, CAP - THIN / 2)
    p.bar(L, 440, CAP * 0.50)
    return p

@glyph("G", 660)
def _G(p):
    _bowl(p, L, 600, 0, CAP, STEM, 38, 322)
    p.stem(590 - STEM / 2, CAP * 0.16, CAP * 0.44, STEM)
    p.bar(390, 600, CAP * 0.44)
    return p

@glyph("H", 650)
def _H(p):
    p.stem(L + STEM / 2, 0, CAP)
    p.stem(580 - STEM / 2, 0, CAP)
    p.bar(L, 580, CAP * 0.50)
    return p

@glyph("I", 300)
def _I(p):
    p.stem(150, 0, CAP)
    return p

@glyph("J", 480)
def _J(p):
    p.stem(340, CAP * 0.26, CAP)
    p.ring(190, CAP * 0.26, 150, CAP * 0.26 - STEM / 3, STEM, 185, 355)
    return p

@glyph("K", 620)
def _K(p):
    p.stem(L + STEM / 2, 0, CAP)
    p.diag(140, CAP * 0.36, 560, CAP)
    p.diag(140, CAP * 0.36, 570, 0)
    return p

@glyph("L", 520)
def _L(p):
    p.stem(L + STEM / 2, 0, CAP)
    p.bar(L, 460, THIN / 2)
    return p

@glyph("M", 780)
def _M(p):
    p.stem(L + STEM / 2, 0, CAP)
    p.stem(710 - STEM / 2, 0, CAP)
    p.diag(L + STEM / 2, CAP, 390, CAP * 0.22)
    p.diag(710 - STEM / 2, CAP, 390, CAP * 0.22)
    return p

@glyph("N", 660)
def _N(p):
    p.stem(L + STEM / 2, 0, CAP)
    p.stem(590 - STEM / 2, 0, CAP)
    p.diag(L + STEM / 2, CAP, 590 - STEM / 2, 0)
    return p

@glyph("O", 700)
def _O(p):
    _bowl(p, L, 630, 0, CAP)
    return p

@glyph("P", 580)
def _P(p):
    p.stem(L + STEM / 2, 0, CAP)
    p.bar(L, 400, CAP - THIN / 2)
    p.bar(L, 400, CAP * 0.44)
    p.ring(400, CAP * 0.72, 110, CAP * 0.28 - STEM / 4, STEM, -90, 90)
    return p

@glyph("Q", 700)
def _Q(p):
    _bowl(p, L, 630, 0, CAP)
    p.diag(400, CAP * 0.24, 620, -60, STEM * 0.95)
    return p

@glyph("R", 610)
def _R(p):
    p.stem(L + STEM / 2, 0, CAP)
    p.bar(L, 390, CAP - THIN / 2)
    p.bar(L, 390, CAP * 0.46)
    p.ring(390, CAP * 0.73, 110, CAP * 0.27 - STEM / 4, STEM, -90, 90)
    p.diag(330, CAP * 0.46, 560, 0)
    return p

@glyph("S", 590)
def _S(p):
    _bowl(p, L, 520, CAP * 0.48, CAP, STEM, 10, 250)
    _bowl(p, L, 520, 0, CAP * 0.52, STEM, 190, 430)
    return p

@glyph("T", 580)
def _T(p):
    p.stem(290, 0, CAP - THIN / 2)
    p.bar(40, 540, CAP - THIN / 2)
    return p

@glyph("U", 650)
def _U(p):
    p.stem(L + STEM / 2, CAP * 0.26, CAP)
    p.stem(580 - STEM / 2, CAP * 0.26, CAP)
    p.ring(325, CAP * 0.26, (580 - L) / 2 - STEM / 2, CAP * 0.26 - STEM / 3,
           STEM, 180, 360)
    return p

@glyph("V", 640)
def _V(p):
    p.diag(50, CAP, 320, 0)
    p.diag(590, CAP, 320, 0)
    return p

@glyph("W", 860)
def _W(p):
    p.diag(40, CAP, 230, 0)
    p.diag(430, CAP * 0.78, 230, 0)
    p.diag(430, CAP * 0.78, 620, 0)
    p.diag(820, CAP, 620, 0)
    return p

@glyph("X", 620)
def _X(p):
    p.diag(50, CAP, 560, 0)
    p.diag(560, CAP, 50, 0)
    return p

@glyph("Y", 610)
def _Y(p):
    p.diag(40, CAP, 300, CAP * 0.46)
    p.diag(560, CAP, 300, CAP * 0.46)
    p.stem(300, 0, CAP * 0.48)
    return p

@glyph("Z", 590)
def _Z(p):
    p.bar(L, 520, CAP - THIN / 2)
    p.bar(L, 520, THIN / 2)
    p.diag(490, CAP - THIN, 110, THIN)
    return p

# --- brackets and marks ---------------------------------------------------

@glyph("[", 320)
def _lb(p):
    p.stem(130, -80, CAP + 60, STEM * 0.85)
    p.bar(130, 290, CAP + 60 - THIN / 2, THIN * 0.9)
    p.bar(130, 290, -80 + THIN / 2, THIN * 0.9)
    return p

@glyph("\\", 440)
def _bs(p):
    p.diag(40, CAP + 60, 400, -60, STEM * 0.85)
    return p

@glyph("]", 320)
def _rb(p):
    p.stem(190, -80, CAP + 60, STEM * 0.85)
    p.bar(30, 190, CAP + 60 - THIN / 2, THIN * 0.9)
    p.bar(30, 190, -80 + THIN / 2, THIN * 0.9)
    return p

@glyph("^", 520)
def _caret(p):
    p.diag(80, CAP * 0.62, 260, CAP, THIN)
    p.diag(440, CAP * 0.62, 260, CAP, THIN)
    return p

@glyph("_", 520)
def _us(p):
    p.bar(20, 500, -110, THIN)
    return p

@glyph("`", 300)
def _grave(p):
    p.diag(80, CAP, 220, CAP - 150, THIN)
    return p

# --- lowercase ------------------------------------------------------------

@glyph("a", 560)
def _a(p):
    _bowl(p, 50, 480, 0, XH)
    p.stem(480 - STEM / 2, 0, XH * 0.52)
    return p

@glyph("b", 570)
def _b(p):
    p.stem(L + STEM / 2, 0, ASC)
    _bowl(p, L, 510, 0, XH)
    return p

@glyph("c", 530)
def _c(p):
    _bowl(p, 50, 480, 0, XH, STEM, 42, 318)
    return p

@glyph("d", 570)
def _d(p):
    p.stem(500 - STEM / 2, 0, ASC)
    _bowl(p, 60, 500, 0, XH)
    return p

@glyph("e", 550)
def _e(p):
    _bowl(p, 50, 490, 0, XH, STEM, 20, 330)
    p.bar(60, 480, XH * 0.54)
    return p

@glyph("f", 360)
def _f(p):
    p.stem(240, 0, ASC - 90)
    p.ring(360, ASC - 90, 120, 110, STEM, 90, 180)
    p.bar(60, 400, XH - THIN / 2)
    return p

@glyph("g", 570)
def _g(p):
    _bowl(p, 50, 490, 0, XH)
    p.stem(490 - STEM / 2, DESC + 120, XH)
    p.ring(310, DESC + 120, 180, 110, STEM, 190, 350)
    return p

@glyph("h", 560)
def _h(p):
    p.stem(L + STEM / 2, 0, ASC)
    p.stem(490 - STEM / 2, 0, XH * 0.62)
    p.ring(300, XH * 0.62, (490 - L) / 2 - STEM / 2, XH * 0.38 - STEM / 3,
           STEM, 0, 180)
    return p

@glyph("i", 260)
def _i(p):
    p.stem(130, 0, XH)
    p.dot(130, ASC - 60, STEM * 0.56)
    return p

@glyph("j", 260)
def _j(p):
    p.stem(140, DESC + 110, XH)
    p.ring(20, DESC + 110, 120, 100, STEM, 250, 360)
    p.dot(140, ASC - 60, STEM * 0.56)
    return p

@glyph("k", 540)
def _k(p):
    p.stem(L + STEM / 2, 0, ASC)
    p.diag(130, XH * 0.38, 490, XH)
    p.diag(130, XH * 0.38, 500, 0)
    return p

@glyph("l", 260)
def _l(p):
    p.stem(130, 0, ASC)
    return p

@glyph("m", 830)
def _m(p):
    p.stem(L + STEM / 2, 0, XH)
    p.stem(390, 0, XH * 0.62)
    p.stem(760 - STEM / 2, 0, XH * 0.62)
    p.ring(230, XH * 0.62, (390 - L) / 2 - STEM / 2, XH * 0.38 - STEM / 3,
           STEM, 0, 180)
    p.ring(575, XH * 0.62, (760 - 390) / 2 - STEM / 2, XH * 0.38 - STEM / 3,
           STEM, 0, 180)
    return p

@glyph("n", 560)
def _n(p):
    p.stem(L + STEM / 2, 0, XH)
    p.stem(490 - STEM / 2, 0, XH * 0.62)
    p.ring(300, XH * 0.62, (490 - L) / 2 - STEM / 2, XH * 0.38 - STEM / 3,
           STEM, 0, 180)
    return p

@glyph("o", 570)
def _o(p):
    _bowl(p, 50, 510, 0, XH)
    return p

@glyph("p", 570)
def _p(p):
    p.stem(L + STEM / 2, DESC, XH)
    _bowl(p, L, 510, 0, XH)
    return p

@glyph("q", 570)
def _q_(p):
    p.stem(500 - STEM / 2, DESC, XH)
    _bowl(p, 60, 500, 0, XH)
    return p

@glyph("r", 400)
def _r(p):
    p.stem(L + STEM / 2, 0, XH)
    p.ring(300, XH * 0.66, 140, XH * 0.34 - STEM / 3, STEM, 60, 180)
    return p

@glyph("s", 500)
def _s(p):
    _bowl(p, 50, 450, XH * 0.46, XH, STEM * 0.95, 10, 250)
    _bowl(p, 50, 450, 0, XH * 0.54, STEM * 0.95, 190, 430)
    return p

@glyph("t", 380)
def _t(p):
    p.stem(180, XH * 0.18, ASC - 60)
    p.bar(40, 330, XH - THIN / 2)
    p.ring(300, XH * 0.18, 120, 100, STEM, 180, 290)
    return p

@glyph("u", 560)
def _u(p):
    p.stem(L + STEM / 2, XH * 0.36, XH)
    p.stem(490 - STEM / 2, 0, XH)
    p.ring(300, XH * 0.36, (490 - L) / 2 - STEM / 2, XH * 0.36 - STEM / 3,
           STEM, 180, 360)
    return p

@glyph("v", 520)
def _v(p):
    p.diag(50, XH, 260, 0, STEM * 0.95)
    p.diag(470, XH, 260, 0, STEM * 0.95)
    return p

@glyph("w", 760)
def _w(p):
    p.diag(40, XH, 200, 0, STEM * 0.9)
    p.diag(370, XH * 0.72, 200, 0, STEM * 0.9)
    p.diag(370, XH * 0.72, 540, 0, STEM * 0.9)
    p.diag(720, XH, 540, 0, STEM * 0.9)
    return p

@glyph("x", 520)
def _x(p):
    p.diag(50, XH, 470, 0, STEM * 0.95)
    p.diag(470, XH, 50, 0, STEM * 0.95)
    return p

@glyph("y", 520)
def _y(p):
    p.diag(50, XH, 280, 0, STEM * 0.95)
    p.diag(470, XH, 180, DESC, STEM * 0.95)
    return p

@glyph("z", 500)
def _z(p):
    p.bar(50, 450, XH - THIN / 2)
    p.bar(50, 450, THIN / 2)
    p.diag(420, XH - THIN, 90, THIN, STEM * 0.9)
    return p

@glyph("{", 340)
def _lbr(p):
    p.ring(230, CAP * 0.78, 120, 120, STEM * 0.8, 90, 180)
    p.ring(230, CAP * 0.16, 120, 120, STEM * 0.8, 180, 270)
    p.stem(110, CAP * 0.16, CAP * 0.78, STEM * 0.8)
    p.ring(-10, CAP * 0.47, 120, 120, STEM * 0.8, -45, 45)
    return p

@glyph("|", 280)
def _bar_(p):
    p.stem(140, -90, CAP + 70, STEM * 0.75)
    return p

@glyph("}", 340)
def _rbr(p):
    p.ring(110, CAP * 0.78, 120, 120, STEM * 0.8, 0, 90)
    p.ring(110, CAP * 0.16, 120, 120, STEM * 0.8, 270, 360)
    p.stem(230, CAP * 0.16, CAP * 0.78, STEM * 0.8)
    p.ring(350, CAP * 0.47, 120, 120, STEM * 0.8, 135, 225)
    return p

@glyph("~", 560)
def _tilde(p):
    p.ring(180, XH * 0.52, 110, 70, THIN, 20, 160)
    p.ring(390, XH * 0.52 + 8, 110, 70, THIN, 200, 340)
    return p


# --- emitting -------------------------------------------------------------
#
# The outlines are rasterised here rather than on the machine, because a
# kernel that rasterises outlines needs floating point and a scanline filler
# in it, and what it actually needs is to put a glyph on the screen. So each
# size is measured once, at build time, and what ships is coverage bytes.

SIZES_KERNEL = [15, 20, 26]
SIZES_USER = [15, 20]

ORDER = [chr(c) for c in range(32, 127)]


def build(size):
    """Every glyph at one size: (w, h, left, top, advance, bytes)."""
    out = []
    scale = size / EM
    for ch in ORDER:
        fn, adv = G[ch]
        p = fn(Pen())
        w, h, left, top, rows = rasterise(p.contours, size)
        data = bytearray()
        for r in rows:
            data.extend(r)
        out.append((w, h, left, top, int(round(adv * scale)), bytes(data)))
    return out


def emit(path, guard, sizes, prefix):
    faces = [(s, build(s)) for s in sizes]

    lines = []
    lines.append("/* Generated by tools/genface.py. Do not edit by hand;")
    lines.append("   the letterforms are drawn in that file. */")
    lines.append("#pragma once" if guard else "")
    lines.append('#include "%s"' % ("types.h" if guard else "nyx.h"))
    lines.append("")
    lines.append("/* One byte of coverage per pixel: 0 is background, 255 is")
    lines.append("   solidly inside the letter. Everything between is an edge. */")
    lines.append("typedef struct {")
    lines.append("    short w, h;          /* of the bitmap, not the glyph */")
    lines.append("    short left, top;     /* where it sits against the pen */")
    lines.append("    short advance;       /* how far the pen then moves */")
    lines.append("    unsigned int at;     /* into the blob below */")
    lines.append("} face_glyph;")
    lines.append("")
    lines.append("#define FACE_FIRST 32")
    lines.append("#define FACE_LAST  126")
    lines.append("#define FACE_COUNT %d" % len(ORDER))
    lines.append("#define FACE_SIZES %d" % len(sizes))
    lines.append("")

    for size, face in faces:
        blob = bytearray()
        metrics = []
        for (w, h, left, top, adv, data) in face:
            metrics.append((w, h, left, top, adv, len(blob)))
            blob.extend(data)

        lines.append("static const unsigned char %s_px_%d[%d] = {"
                     % (prefix, size, max(1, len(blob))))
        row = []
        for i, b in enumerate(blob):
            row.append("%d," % b)
            if len(row) == 24:
                lines.append("    " + "".join(row))
                row = []
        if row:
            lines.append("    " + "".join(row))
        if not blob:
            lines.append("    0,")
        lines.append("};")
        lines.append("")

        lines.append("static const face_glyph %s_g_%d[%d] = {" % (prefix, size, len(metrics)))
        for i, (w, h, left, top, adv, at) in enumerate(metrics):
            ch = ORDER[i]
            shown = ch if ch not in "\'\"" else "\\" + ch
            lines.append("    { %d, %d, %d, %d, %d, %d },   /* '%s' */"
                         % (w, h, left, top, adv, at, shown))
        lines.append("};")
        lines.append("")

    lines.append("typedef struct {")
    lines.append("    short size;")
    lines.append("    const face_glyph *glyphs;")
    lines.append("    const unsigned char *pixels;")
    lines.append("} face_t;")
    lines.append("")
    lines.append("static const face_t %s_faces[FACE_SIZES] = {" % prefix)
    for size, _ in faces:
        lines.append("    { %d, %s_g_%d, %s_px_%d }," % (size, prefix, size, prefix, size))
    lines.append("};")
    lines.append("")

    with open(path, "w", newline="") as f:
        f.write("\n".join(lines))
    return sum(len(d) for _, face in faces for (*_m, d) in face)


def main():
    if "--preview" in sys.argv:
        size = 22
        for ch in "Hamburgefonstiv 0123456789":
            fn, _ = G[ch]
            art, w, h, l, t = preview(fn(Pen()).contours, size)
            print("%s  %dx%d" % (ch, w, h))
            for row in art:
                print("  |" + row + "|")
        return 0

    n1 = emit("include/face.h", True, SIZES_KERNEL, "face")
    n2 = emit("userland/face.h", False, SIZES_USER, "face")
    print("  include/face.h    %d sizes, %d bytes of coverage" % (len(SIZES_KERNEL), n1))
    print("  userland/face.h   %d sizes, %d bytes of coverage" % (len(SIZES_USER), n2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
