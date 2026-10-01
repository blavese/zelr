"""Writes userland/h264tab.h: the numbers an H.264 decoder needs, read out of
the reference decoder ITU-T publishes (JM, the software of H.264.2).

Only numbers are taken, never code. They are what the standard's tables
hold, and the reference software is where they can be read without typing
two thousand of them in by hand:

  - the CABAC context initialisation (m, n) for every context a 4:2:0
    stream uses (ctxIdx 0-459 and 1012-1015), for I slices and for each of
    the three cabac_init_idc of P and B slices. JM keeps them grouped by
    syntax element; each group is put back at the ctxIdx the standard gives
    it (Table 9-34), by the way JM's decoder indexes it, set out below;
  - the arithmetic decoder's rangeTabLPS and transIdxLPS (Tables 9-44, 9-45);
  - the CAVLC codes: coeff_token for each nC range and for chroma DC,
    total_zeros and run_before (Tables 9-5, 9-7 to 9-10);
  - where an 8x8 block's positions take their contexts (Table 9-43);
  - the zig-zag and field scans, the deblocking filter's alpha, beta and
    tC0 (Tables 8-16, 8-17), the dequantisation scales, the chroma QP map
    (Table 8-15) and the default scaling lists (Tables 7-3, 7-4).

Every table is checked as it is read: the CABAC contexts land on each
ctxIdx exactly once and agree with values known from the standard, the
variable-length codes are prefix-free with the counts the standard gives,
and the scans are permutations.

  python tools/mkh264.py            (fetches jm19.0.zip)
  python tools/mkh264.py jm19.0.zip
"""
import io
import os
import re
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "userland", "h264tab.h")
URL = "https://iphome.hhi.de/suehring/tml/download/jm19.0.zip"


def fetch():
    try:
        return urllib.request.urlopen(urllib.request.Request(URL, headers={"User-Agent": "mkh264"}),
                                      timeout=120).read()
    except Exception as e:
        # The server leaves out an intermediate certificate, which Python's
        # own store cannot find and the system's (through curl) can. Still
        # verified, either way.
        print("urllib could not fetch it (%s); asking curl" % e)
        r = subprocess.run(["curl", "-sSf", URL], stdout=subprocess.PIPE)
        if r.returncode:
            sys.exit("could not fetch %s" % URL)
        return r.stdout


def source(z, name):
    return z.read("JM/" + name).decode("latin-1")


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def c_braces(text, name):
    """The braces after `name ... =`, as nested lists of ints."""
    s = strip_comments(text)
    m = re.search(r"\b" + re.escape(name) + r"\b[^=;{]*=\s*\{", s)
    if not m:
        sys.exit("no table %s" % name)
    at = m.end() - 1
    depth, end = 0, at
    for i in range(at, len(s)):
        if s[i] == "{":
            depth += 1
        elif s[i] == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    body = s[at:end]
    # JM writes an unused place as CTX_UNUSED, which it defines as (0, 64):
    # but (0, 64) is also a real context's value, written out (ctxIdx 254),
    # so the places it marks are told apart by a pair no context has.
    body = body.replace("CTX_UNUSED", "{999,999}").replace("CTX_UNDEF", "{999,999}")
    tokens = re.findall(r"[{}]|-?\d+", body)
    stack, cur = [], None
    for t in tokens:
        if t == "{":
            stack.append([])
        elif t == "}":
            done = stack.pop()
            if stack:
                stack[-1].append(done)
            else:
                cur = done
        else:
            stack[-1].append(int(t))
    return cur


def flat(x):
    if isinstance(x, list):
        out = []
        for v in x:
            out += flat(v)
        return out
    return [x]


# --- CABAC ---------------------------------------------------------------------------------

UNUSED = (999, 999)


def cabac(ctxsrc):
    """ctx[model][ctxIdx] = (m, n) or None; model 0 is I, 1-3 cabac_init_idc 0-2."""
    T = {}
    for name in ("MB_TYPE", "B8_TYPE", "MV_RES", "REF_NO", "TRANSFORM_SIZE", "DELTA_QP", "MB_AFF",
                 "IPR", "CIPR", "CBP", "BCBP", "MAP", "LAST", "ONE", "ABS", "FLD_MAP", "FLD_LAST"):
        T[name] = (c_braces(ctxsrc, "INIT_%s_I" % name), c_braces(ctxsrc, "INIT_%s_P" % name))

    ctx = [[None] * 1024 for _ in range(4)]

    def put(model, idx, mn, what):
        mn = tuple(mn)
        if mn == UNUSED:
            sys.exit("ctxIdx %d (%s) would come from an unused place in JM's table" % (idx, what))
        if ctx[model][idx] is not None:
            sys.exit("ctxIdx %d is given twice (%s)" % (idx, what))
        ctx[model][idx] = mn

    def models(name, i_too=True, pb_too=True):
        I, P = T[name]
        out = []
        if i_too:
            out.append((0, I[0]))
        if pb_too:
            for k in range(3):
                out.append((k + 1, P[k]))
        return out

    # mb_type for SI and I (0-10): JM row 1 is SI, row 0 is I. I slices only.
    for model, t in models("MB_TYPE", pb_too=False):
        for k in range(3):
            put(model, 0 + k, t[1][k], "mb_type SI prefix")
        for k in range(3):
            put(model, 3 + k, t[0][k], "mb_type I, bin 0")
        for k in range(5):
            put(model, 6 + k, t[0][4 + k], "mb_type I, later bins")
    # P and B (11-35): row 1 P, row 2 B; the B intra suffix's later bins
    # (33-35) are read from row 1, which holds the same numbers.
    for model, t in models("MB_TYPE", i_too=False):
        p, b = t[1], t[2]
        for k in range(3):
            put(model, 11 + k, p[k], "mb_skip_flag P")
        for k in range(3):
            put(model, 14 + k, p[4 + k], "mb_type P prefix")
        for k in range(4):
            put(model, 17 + k, p[7 + k], "mb_type P intra suffix")
        for k in range(3):
            put(model, 24 + k, b[7 + k], "mb_skip_flag B")
        for k in range(3):
            put(model, 27 + k, b[k], "mb_type B, bin 0")
        for k in range(3):
            put(model, 30 + k, b[4 + k], "mb_type B, later bins")
        for k in range(3):
            put(model, 33 + k, p[8 + k], "mb_type B intra suffix")
    # sub_mb_type: P from row 0 at 1, 3, 4; B from row 1 at 0-3.
    for model, t in models("B8_TYPE", i_too=False):
        for k, j in enumerate((1, 3, 4)):
            put(model, 21 + k, t[0][j], "sub_mb_type P")
        for k in range(4):
            put(model, 36 + k, t[1][k], "sub_mb_type B")
    # mvd: for each component c, bin 0 from row 0 at 5c+{0,2,3}, later bins
    # from row 1 at 5c+0..3.
    for model, t in models("MV_RES", i_too=False):
        for c in range(2):
            base = 40 + 7 * c
            for k, j in enumerate((0, 2, 3)):
                put(model, base + k, t[0][5 * c + j], "mvd bin 0")
            for k in range(4):
                put(model, base + 3 + k, t[1][5 * c + k], "mvd later bins")
    # ref_idx: row 0 (JM's decoder never reads row 1).
    for model, t in models("REF_NO", i_too=False):
        for k in range(6):
            put(model, 54 + k, t[0][k], "ref_idx")
    for model, t in models("DELTA_QP"):
        for k in range(4):
            put(model, 60 + k, t[0][k], "mb_qp_delta")
    for model, t in models("CIPR"):
        for k in range(4):
            put(model, 64 + k, t[0][k], "intra_chroma_pred_mode")
    for model, t in models("IPR"):
        put(model, 68, t[0][0], "prev_intra_pred_mode_flag")
        put(model, 69, t[0][1], "rem_intra_pred_mode")
    for model, t in models("MB_AFF"):
        for k in range(3):
            put(model, 70 + k, t[0][k], "mb_field_decoding_flag")
    for model, t in models("CBP"):
        for k in range(4):
            put(model, 73 + k, t[0][k], "coded_block_pattern luma")
            put(model, 77 + k, t[1][k], "coded_block_pattern chroma, bin 0")
            put(model, 81 + k, t[2][k], "coded_block_pattern chroma, bin 1")
    for model, t in models("TRANSFORM_SIZE"):
        for k in range(3):
            put(model, 399 + k, t[0][k], "transform_size_8x8_flag")

    # Residual blocks. ctxBlockCat: 0 luma DC of 16x16, 1 its AC, 2 luma 4x4,
    # 3 chroma DC, 4 chroma AC, 5 luma 8x8. JM's rows for each (its
    # type2ctx_* tables), and whether JM counts positions from the DC (an
    # AC block's first coefficient is at 1 in JM, at 0 in the standard).
    cbf_row = {0: 0, 1: 1, 2: 4, 3: 5, 4: 6, 5: 2}
    map_row = {0: 0, 1: 1, 2: 5, 3: 6, 4: 7, 5: 2}
    lvl_row = cbf_row
    from_one = {0: 0, 1: 1, 2: 0, 3: 0, 4: 1, 5: 0}
    sig_n = {0: 15, 1: 14, 2: 15, 3: 3, 4: 14}
    cat_off_sig = {0: 0, 1: 15, 2: 29, 3: 44, 4: 47}
    cat_off_lvl = {0: 0, 1: 10, 2: 20, 3: 30, 4: 39}
    for model, t in models("BCBP"):
        for cat in range(5):
            for k in range(4):
                put(model, 85 + 4 * cat + k, t[cbf_row[cat]][k], "coded_block_flag")
        for k in range(4):
            put(model, 1012 + k, t[cbf_row[5]][k], "coded_block_flag 8x8")
    for name, frame_base, field_base, b5f, b5i, n5 in (("MAP", 105, None, 402, None, 15),
                                                       ("LAST", 166, None, 417, None, 9),
                                                       ("FLD_MAP", None, 277, None, 436, 15),
                                                       ("FLD_LAST", None, 338, None, 451, 9)):
        base = frame_base if frame_base is not None else field_base
        base5 = b5f if b5f is not None else b5i
        for model, t in models(name):
            for cat in range(5):
                for k in range(sig_n[cat]):
                    put(model, base + cat_off_sig[cat] + k, t[map_row[cat]][k + from_one[cat]], name)
            for k in range(n5):
                put(model, base5 + k, t[map_row[5]][k], name + " 8x8")
    for model, t in models("ONE"):
        a = T["ABS"][0][0] if model == 0 else T["ABS"][1][model - 1]
        for cat in range(5):
            for k in range(5):
                put(model, 227 + cat_off_lvl[cat] + k, t[lvl_row[cat]][k], "coeff_abs_level_minus1 bin 0")
            for k in range(4 if cat == 3 else 5):
                put(model, 227 + cat_off_lvl[cat] + 5 + k, a[lvl_row[cat]][k], "coeff_abs_level_minus1 later bins")
        for k in range(5):
            put(model, 426 + k, t[lvl_row[5]][k], "coeff_abs_level_minus1 8x8 bin 0")
            put(model, 431 + k, a[lvl_row[5]][k], "coeff_abs_level_minus1 8x8 later bins")
    return ctx


def check_cabac(ctx):
    # Which contexts each kind of slice has: 0-10 only I, 11-59 only P and B,
    # 276 (end_of_slice_flag) none, since it is not initialised from (m, n).
    for idx in list(range(0, 460)) + list(range(1012, 1016)):
        for model in range(4):
            want = idx != 276 and not (model == 0 and 11 <= idx <= 59) and not (model > 0 and idx <= 10)
            if (ctx[model][idx] is not None) != want:
                sys.exit("ctxIdx %d in model %d is %s" % (idx, model, "missing" if want else "given"))
    for model in range(4):
        for idx in list(range(460, 1012)) + list(range(1016, 1024)):
            if ctx[model][idx] is not None:
                sys.exit("ctxIdx %d was not expected" % idx)
    # Values the standard gives that do not depend on reading JM right.
    known = {
        (0, 0): (20, -15), (0, 3): (20, -15), (0, 6): (-28, 127), (0, 10): (7, 51),
        (1, 40): (-3, 69), (1, 41): (-6, 81), (1, 42): (-11, 96), (1, 43): (6, 55),
        (1, 46): (2, 88), (1, 47): (0, 58), (1, 53): (0, 88),
        (0, 60): (0, 41), (0, 61): (0, 63), (1, 62): (0, 63), (3, 63): (0, 63),
        (0, 64): (-9, 83), (0, 65): (4, 86), (0, 66): (0, 97), (0, 67): (-7, 72),
        (0, 68): (13, 41), (0, 69): (3, 62),
        (0, 70): (0, 11), (0, 71): (1, 55), (0, 72): (0, 69),
        (0, 399): (31, 21), (0, 400): (31, 31), (0, 401): (25, 50),
        (1, 11): (23, 33), (1, 14): (1, 9), (1, 20): (1, 62), (1, 24): (18, 64), (1, 27): (26, 67),
        (1, 33): (-13, 78), (1, 35): (1, 62),
    }
    for (model, idx), mn in known.items():
        if ctx[model][idx] != mn:
            sys.exit("ctxIdx %d in model %d is %s, not %s" % (idx, model, ctx[model][idx], mn))
    for model in range(4):
        for idx, mn in enumerate(ctx[model]):
            if mn is not None and not (-128 <= mn[0] <= 127 and -128 <= mn[1] <= 127):
                sys.exit("ctxIdx %d has (%d, %d)" % ((idx,) + mn))


# --- variable-length codes -----------------------------------------------------------------

def codes_from(lentab, codtab, width, height):
    """(code, length, i, j) for every code, i along a row, j down."""
    out = []
    for j in range(height):
        for i in range(width):
            n = lentab[j][i] if i < len(lentab[j]) else 0     # C pads a short row
            if n:
                out.append((codtab[j][i], n, i, j))
    return out


def check_prefix_free(codes, what, count=None, full=True):
    seen = []
    for code, n, _, _ in codes:
        if code >= (1 << n):
            sys.exit("%s: code %d does not fit %d bits" % (what, code, n))
        seen.append(format(code, "0%db" % n))
    for a in seen:
        for b in seen:
            if a is not b and b.startswith(a):
                sys.exit("%s: %s is the start of %s" % (what, a, b))
    kraft = sum(2.0 ** -len(s) for s in seen)
    if kraft > 1.0 + 1e-12 or (full and kraft < 1.0 - 1e-12):
        sys.exit("%s: Kraft sum %r" % (what, kraft))
    if count is not None and len(seen) != count:
        sys.exit("%s: %d codes, not %d" % (what, len(seen), count))


def vlc(src):
    s = strip_comments(src)
    # The three coeff_token tables keyed by nC, and chroma DC's.
    def tables(fn):
        at = s.index(fn)
        return c_braces(s[at:], "lentab"), c_braces(s[at:], "codtab")
    ct_len, ct_cod = tables("readSyntaxElement_NumCoeffTrailingOnes(")
    dc_len, dc_cod = tables("readSyntaxElement_NumCoeffTrailingOnesChromaDC(")
    tz_len, tz_cod = tables("readSyntaxElement_TotalZeros(")
    tzc_len, tzc_cod = tables("readSyntaxElement_TotalZerosChromaDC(")
    rb_len, rb_cod = tables("readSyntaxElement_Run(")
    out = {}
    for v in range(3):
        c = codes_from(ct_len[v], ct_cod[v], 17, 4)
        check_prefix_free(c, "coeff_token nC table %d" % v, 62, full=False)
        out["coeff_token_%d" % v] = c
    c = codes_from(dc_len[0], dc_cod[0], 17, 4)
    check_prefix_free(c, "coeff_token chroma DC", 14, full=False)
    out["coeff_token_dc"] = c
    tz = []
    for v in range(15):
        c = codes_from([tz_len[v]], [tz_cod[v]], 16, 1)
        # The table for one coefficient leaves its all-zero code unused, as
        # the standard's does; the rest fill their space exactly.
        check_prefix_free(c, "total_zeros for %d coefficients" % (v + 1), 16 - v, full=v > 0)
        tz.append(c)
    out["total_zeros"] = tz
    tzc = []
    for v in range(3):
        c = codes_from([tzc_len[0][v]], [tzc_cod[0][v]], 16, 1)
        check_prefix_free(c, "chroma DC total_zeros for %d" % (v + 1), 4 - v, full=True)
        tzc.append(c)
    out["total_zeros_dc"] = tzc
    rb = []
    for v in range(7):
        c = codes_from([rb_len[v]], [rb_cod[v]], 16, 1)
        check_prefix_free(c, "run_before with %s zeros left" % (v + 1 if v < 6 else ">6"),
                          v + 2 if v < 6 else 15, full=v < 6)
        rb.append(c)
    out["run_before"] = rb
    return out


# --- writing ---------------------------------------------------------------------------------

def c_array(name, ctype, values, per=16):
    lines = ["static const %s %s[%d] = {" % (ctype, name, len(values))]
    for i in range(0, len(values), per):
        lines.append("    " + ", ".join(str(v) for v in values[i:i + per]) + ",")
    lines.append("};")
    return lines


def main():
    raw = open(sys.argv[1], "rb").read() if len(sys.argv) > 1 else fetch()
    z = zipfile.ZipFile(io.BytesIO(raw))

    ctx = cabac(source(z, "lcommon/inc/ctx_tables.h"))
    check_cabac(ctx)

    bia = source(z, "ldecod/inc/biaridecod.h")
    rlps = c_braces(bia, "rLPS_table_64x4")
    nlps = flat(c_braces(bia, "AC_next_state_LPS_64"))
    if len(rlps) != 64 or any(len(r) != 4 for r in rlps) or len(nlps) != 64:
        sys.exit("rangeTabLPS or transIdxLPS is not 64 long")
    if rlps[0] != [128, 176, 208, 240] or rlps[63] != [2, 2, 2, 2] or nlps[0] != 0 or nlps[63] != 63:
        sys.exit("rangeTabLPS or transIdxLPS is not the standard's")

    codes = vlc(source(z, "ldecod/src/vlc.c"))

    mbh = source(z, "ldecod/inc/macroblock.h")
    scans = {}
    for name, n in (("SNGL_SCAN", 16), ("FIELD_SCAN", 16), ("SNGL_SCAN8x8", 64), ("FIELD_SCAN8x8", 64)):
        pairs = c_braces(mbh, name)
        side = 4 if n == 16 else 8
        order = [p[1] * side + p[0] for p in pairs]       # JM's pairs are (x, y)
        if sorted(order) != list(range(n)):
            sys.exit("%s is not a permutation" % name)
        scans[name] = order
    if scans["SNGL_SCAN"][:6] != [0, 1, 4, 8, 5, 2]:
        sys.exit("the zig-zag scan does not start the standard's way")

    lf = source(z, "ldecod/inc/loop_filter.h")
    alpha = flat(c_braces(lf, "ALPHA_TABLE"))
    beta = flat(c_braces(lf, "BETA_TABLE"))
    clip = c_braces(lf, "CLIP_TAB")
    if len(alpha) != 52 or len(beta) != 52 or len(clip) != 52 or alpha[51] != 255 or beta[51] != 18:
        sys.exit("the deblocking tables are not the standard's")

    qh = source(z, "ldecod/inc/quant.h")
    dq4 = c_braces(qh, "dequant_coef")
    dq8 = c_braces(qh, "dequant_coef8")
    if flat(dq4[0])[:4] != [10, 13, 10, 13] or len(dq8) != 6:
        sys.exit("the dequantisation scales are not the standard's")
    qpc = flat(c_braces(source(z, "ldecod/inc/block.h"), "QP_SCALE_CR"))
    if len(qpc) != 52 or qpc[29] != 29 or qpc[51] != 39:
        sys.exit("the chroma QP table is not the standard's")
    qsrc = source(z, "ldecod/src/quant.c")
    d4i, d4p = flat(c_braces(qsrc, "quant_intra_default")), flat(c_braces(qsrc, "quant_inter_default"))
    d8i, d8p = flat(c_braces(qsrc, "quant8_intra_default")), flat(c_braces(qsrc, "quant8_inter_default"))
    if d4i[:3] != [6, 13, 20] or d4p[:3] != [10, 14, 20] or len(d8i) != 64 or len(d8p) != 64:
        sys.exit("the default scaling lists are not the standard's")

    # Table 9-4, CAVLC's coded_block_pattern: for 4:2:0, intra and inter.
    ncbp = c_braces(source(z, "ldecod/inc/vlc.h"), "NCBP")
    cbp_intra = [ncbp[1][k][0] for k in range(48)]
    cbp_inter = [ncbp[1][k][1] for k in range(48)]
    if sorted(cbp_intra) != list(range(48)) or sorted(cbp_inter) != list(range(48)):
        sys.exit("the coded_block_pattern table is not two permutations")
    if cbp_intra[:4] != [47, 31, 15, 0] or cbp_inter[:4] != [0, 16, 1, 2]:
        sys.exit("the coded_block_pattern table is not the standard's")

    cab = source(z, "ldecod/src/cabac.c")
    p8 = flat(c_braces(cab, "pos2ctx_map8x8"))
    p8i = flat(c_braces(cab, "pos2ctx_map8x8i"))
    l8 = flat(c_braces(cab, "pos2ctx_last8x8"))
    if len(p8) != 64 or len(p8i) != 64 or len(l8) != 64 or max(p8) != 14 or max(l8) != 8:
        sys.exit("the 8x8 context positions are not the standard's")

    o = ["/* Generated by tools/mkh264.py from the numbers in ITU-T's reference decoder",
         "   (JM 19.0, the software of H.264.2). Do not edit. */",
         "#pragma once", ""]
    for model, name in enumerate(("I", "PB0", "PB1", "PB2")):
        vals = []
        for idx in range(1024):
            mn = ctx[model][idx] or (0, 0)
            vals += [mn[0], mn[1]]
        o += c_array("H264_CABAC_%s" % name, "signed char", vals, 20)
    o += c_array("H264_RANGE_LPS", "unsigned char", flat(rlps), 16)
    o += c_array("H264_NEXT_LPS", "unsigned char", nlps, 16)

    def vlc_table(name, c):
        o.append("static const h264_vlc %s[%d] = {" % (name, len(c)))
        for code, n, i, j in sorted(c, key=lambda x: (x[1], x[0])):
            o.append("    { %d, %d, %d, %d }," % (code, n, i, j))
        o.append("};")

    o += ["typedef struct { unsigned short code; unsigned char len, a, b; } h264_vlc;",
          "/* coeff_token: a = TotalCoeff, b = TrailingOnes; total_zeros and run_before:",
          "   a = the value. Sorted by length. */"]
    for v in range(3):
        vlc_table("H264_COEFF_TOKEN_%d" % v, codes["coeff_token_%d" % v])
    vlc_table("H264_COEFF_TOKEN_DC", codes["coeff_token_dc"])
    for v, c in enumerate(codes["total_zeros"]):
        vlc_table("H264_TOTAL_ZEROS_%d" % (v + 1), c)
    for v, c in enumerate(codes["total_zeros_dc"]):
        vlc_table("H264_TOTAL_ZEROS_DC_%d" % (v + 1), c)
    for v, c in enumerate(codes["run_before"]):
        vlc_table("H264_RUN_BEFORE_%d" % (v + 1), c)
    o.append("static const h264_vlc *const H264_TOTAL_ZEROS[15] = {")
    o.append("    " + ", ".join("H264_TOTAL_ZEROS_%d" % (v + 1) for v in range(15)) + ",")
    o.append("};")
    o.append("static const unsigned char H264_TOTAL_ZEROS_N[15] = { %s };"
             % ", ".join(str(len(c)) for c in codes["total_zeros"]))
    o.append("static const h264_vlc *const H264_RUN_BEFORE[7] = {")
    o.append("    " + ", ".join("H264_RUN_BEFORE_%d" % (v + 1) for v in range(7)) + ",")
    o.append("};")
    o.append("static const unsigned char H264_RUN_BEFORE_N[7] = { %s };"
             % ", ".join(str(len(c)) for c in codes["run_before"]))

    o += c_array("H264_ZIGZAG4", "unsigned char", scans["SNGL_SCAN"])
    o += c_array("H264_FIELD4", "unsigned char", scans["FIELD_SCAN"])
    o += c_array("H264_ZIGZAG8", "unsigned char", scans["SNGL_SCAN8x8"])
    o += c_array("H264_FIELD8", "unsigned char", scans["FIELD_SCAN8x8"])
    o += c_array("H264_SIG8_FRAME", "unsigned char", p8)
    o += c_array("H264_SIG8_FIELD", "unsigned char", p8i)
    o += c_array("H264_LAST8", "unsigned char", l8)
    o += c_array("H264_ALPHA", "unsigned char", alpha)
    o += c_array("H264_BETA", "unsigned char", beta)
    o += c_array("H264_TC0", "unsigned char", flat([r[1:4] if len(r) == 5 else r[:3] for r in clip]), 15)
    o += c_array("H264_DEQUANT4", "unsigned char", flat(dq4))
    o += c_array("H264_DEQUANT8", "unsigned char", flat(dq8))
    o += c_array("H264_QP_CHROMA", "unsigned char", qpc)
    o += c_array("H264_DEFAULT4_INTRA", "unsigned char", d4i)
    o += c_array("H264_DEFAULT4_INTER", "unsigned char", d4p)
    o += c_array("H264_DEFAULT8_INTRA", "unsigned char", d8i)
    o += c_array("H264_DEFAULT8_INTER", "unsigned char", d8p)
    o += c_array("H264_CBP_INTRA", "unsigned char", cbp_intra)
    o += c_array("H264_CBP_INTER", "unsigned char", cbp_inter)
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(o) + "\n")
    used = sum(1 for m in range(4) for v in ctx[m] if v is not None)
    print("wrote %s: %d contexts placed, %d variable-length codes" %
          (OUT, used, sum(len(v) if isinstance(v, list) and v and isinstance(v[0], tuple) else
                          sum(len(x) for x in v) for v in codes.values())))


if __name__ == "__main__":
    main()
