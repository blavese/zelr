#!/usr/bin/env python3
# The constant tables userland/webp.h decodes with, taken out of the text of
# the two specifications rather than typed in or copied from a decoder:
# RFC 6386 (VP8, the lossy half) and RFC 9649 (the WebP container and the
# lossless half). Writes userland/webptab.h.
#
#   python tools/genwebptab.py                    # fetches both from rfc-editor.org
#   python tools/genwebptab.py rfc6386.txt rfc9649.txt
#
# Every table is found by the declaration the RFC prints in front of it,
# read up to its closing brace, and counted: a table with a number missing
# or one too many stops the script instead of reaching the header. Page
# headers and footers (RFC 6386 is paginated) are removed first, since they
# carry numbers of their own. Names in the trees (-DC_PRED, -dct_eob) are
# given the values of the enumerations the RFC declares them in.
#
# The one table taken from RFC 6386's reference source (section 20) is the
# zigzag order, which the prose names but does not list: it is generated
# here by walking the anti-diagonals of a 4x4 block and then checked against
# that listing. Nothing else is read from section 20.
import hashlib
import os
import re
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
URL = "https://www.rfc-editor.org/rfc/rfc{}.txt"


def fetch(number, path):
    if path:
        with open(path, "rb") as f:
            raw = f.read()
    else:
        with urllib.request.urlopen(URL.format(number), timeout=60) as r:
            raw = r.read()
    return raw.decode("utf-8-sig"), hashlib.sha256(raw).hexdigest()


def unpage(text):
    """The text with RFC 6386's page breaks taken out."""
    keep = []
    for line in text.replace("\f", "\n").split("\n"):
        if re.match(r"^Bankoski, et al\.\s+Informational\s+\[Page \d+\]\s*$", line):
            continue
        if re.match(r"^RFC 6386\s+VP8 Data Format and Decoding Guide\s+November 2011\s*$", line):
            continue
        keep.append(line)
    return "\n".join(keep)


def uncomment(text):
    return re.sub(r"/\*.*?\*/", " ", text, flags=re.S)


def body(text, declaration, before=None):
    """What is inside the braces after a declaration's '='. Counted to the
    brace that balances the first, since one of the RFC's trees ends in '}'
    with no semicolon."""
    m = re.search(declaration, text)
    if not m:
        sys.exit("not found: " + declaration)
    if before is not None and m.start() > before:
        sys.exit("found only past the prose: " + declaration)
    rest = uncomment(text[m.end():])
    start = rest.index("{")
    depth = 0
    for i in range(start, len(rest)):
        if rest[i] == "{":
            depth += 1
        elif rest[i] == "}":
            depth -= 1
            if depth == 0:
                return rest[start + 1:i]
    sys.exit("unbalanced: " + declaration)


def numbers(text, declaration, count, before=None):
    got = [int(v) for v in re.findall(r"-?\d+", body(text, declaration, before))]
    if len(got) != count:
        sys.exit("%s: %d numbers, wanted %d" % (declaration, len(got), count))
    return got


def enum(text, declaration):
    """The values of a typedef'd enumeration, by name."""
    m = re.search(r"typedef enum\s*\{([^{}]*)\}\s*" + declaration + r"\s*;", text)
    if not m:
        sys.exit("no enumeration " + declaration)
    values, n = {}, 0
    for item in uncomment(m.group(1)).split(","):
        item = item.strip()
        if not item:
            continue
        if "=" in item:              # num_uv_modes = B_PRED: a count, not a value
            name, alias = [s.strip() for s in item.split("=")]
            values[name] = values[alias]
            continue
        values[item] = n
        n += 1
    return values


def tree(text, declaration, names, count, before):
    out = []
    for tok in re.findall(r"-?\s*[A-Za-z_][A-Za-z_0-9]*|-?\d+", body(text, declaration, before)):
        tok = tok.replace(" ", "")
        neg = tok.startswith("-")
        word = tok.lstrip("-")
        v = int(word) if word.isdigit() else names[word]
        out.append(-v if neg else v)
    if len(out) != count:
        sys.exit("%s: %d entries, wanted %d" % (declaration, len(out), count))
    return out


def zigzag():
    """The 4x4 zigzag, walked: along each anti-diagonal, turning each time."""
    order = []
    for d in range(7):
        cells = [(r, d - r) for r in range(4) if 0 <= d - r < 4]
        if d % 2 == 0:
            cells.reverse()          # up and to the right on the even ones
        order += [r * 4 + c for r, c in cells]
    return order


def c_array(ctype, name, values, dims, where):
    """A C declaration, nested to the given dimensions."""
    def nest(vals, dims, depth):
        pad = "    " * depth
        if len(dims) == 1 and len(vals) <= 16:
            return pad + "{ " + ", ".join(str(v) for v in vals) + " }"
        if len(dims) == 1:
            rows = [pad + "    " + ", ".join(str(v) for v in vals[i:i + 16])
                    for i in range(0, len(vals), 16)]
            return pad + "{\n" + ",\n".join(rows) + "\n" + pad + "}"
        step = len(vals) // dims[0]
        inner = [nest(vals[i * step:(i + 1) * step], dims[1:], depth + 1) for i in range(dims[0])]
        return pad + "{\n" + ",\n".join(inner) + "\n" + pad + "}"
    decl = "static const %s %s%s = " % (ctype, name, "".join("[%d]" % d for d in dims))
    return "/* %s */\n%s%s;\n" % (where, decl, nest(values, dims, 0).lstrip())


def main():
    args = sys.argv[1:]
    vp8, vp8_sum = fetch(6386, args[0] if len(args) > 0 else None)
    webp, webp_sum = fetch(9649, args[1] if len(args) > 1 else None)
    vp8 = unpage(vp8)
    # Everything but the zigzag comes from the prose, which ends where the
    # reference source (section 20) begins.
    prose = vp8.index("\n20.  Attachment One: Reference Decoder Source Code")

    modes = enum(vp8, "intra_mbmode")
    bmodes = enum(vp8, "intra_bmode")
    tokens = enum(vp8, "dct_token")

    out = []
    out.append(c_array("unsigned char", "VP8_KF_YMODE_PROB",
                       numbers(vp8, r"const Prob kf_ymode_prob \[num_ymodes - 1\] =", 4, prose),
                       [4], "RFC 6386 section 11.2: key frame luma mode probabilities"))
    out.append(c_array("signed char", "VP8_KF_YMODE_TREE",
                       tree(vp8, r"const tree_index kf_ymode_tree \[2 \* \(num_ymodes - 1\)\] =", modes, 8, prose),
                       [8], "RFC 6386 section 11.2: the key frame luma mode tree"))
    out.append(c_array("unsigned char", "VP8_KF_UV_MODE_PROB",
                       numbers(vp8, r"const Prob kf_uv_mode_prob \[num_uv_modes - 1\] =", 3, prose),
                       [3], "RFC 6386 section 11.4: key frame chroma mode probabilities"))
    out.append(c_array("signed char", "VP8_UV_MODE_TREE",
                       tree(vp8, r"const tree_index uv_mode_tree \[2 \* \(num_uv_modes - 1\)\] =", modes, 6, prose),
                       [6], "RFC 6386 section 11.4: the chroma mode tree"))
    out.append(c_array("signed char", "VP8_BMODE_TREE",
                       tree(vp8, r"const tree_index bmode_tree \[2 \* \(num_intra_bmodes - 1\)\] =", bmodes, 18, prose),
                       [18], "RFC 6386 section 11.2: the subblock mode tree"))
    out.append(c_array("unsigned char", "VP8_KF_BMODE_PROB",
                       numbers(vp8, r"const Prob kf_bmode_prob \[num_intra_bmodes\] \[num_intra_bmodes\]\s*"
                                    r"\[num_intra_bmodes-1\] =", 900, prose),
                       [10, 10, 9], "RFC 6386 section 11.5: subblock mode probabilities, [above][left]"))
    out.append(c_array("signed char", "VP8_SEGMENT_TREE",
                       tree(vp8, r"const tree_index mb_segment_tree \[2 \* \(4-1\)\] =", {}, 6, prose),
                       [6], "RFC 6386 section 10: the segment id tree"))
    out.append(c_array("signed char", "VP8_COEFF_TREE",
                       tree(vp8, r"const tree_index coeff_tree \[2 \* \(num_dct_tokens - 1\)\] =", tokens, 22, prose),
                       [22], "RFC 6386 section 13.2: the coefficient token tree"))
    for i, count in enumerate([1, 2, 3, 4, 5, 11]):
        vals = numbers(vp8, r"const Prob Pcat%d\[\] =" % (i + 1), count + 1, prose)
        out.append(c_array("unsigned char", "VP8_PCAT%d" % (i + 1), vals, [count + 1],
                           "RFC 6386 section 13.2: extra bits of dct_cat%d, ending in 0" % (i + 1)))
    out.append(c_array("short", "VP8_DCT_CAT_BASE",
                       numbers(vp8, r"int categoryBase\[6\] =", 6, prose),
                       [6], "RFC 6386 section 13.3: the smallest value of dct_cat1..dct_cat6"))
    out.append(c_array("unsigned char", "VP8_COEFF_BANDS",
                       numbers(vp8, r"const int coeff_bands \[16\] =", 16, prose),
                       [16], "RFC 6386 section 13.3: coefficient position to band"))
    out.append(c_array("unsigned char", "VP8_COEFF_UPDATE_PROBS",
                       numbers(vp8, r"const Prob coeff_update_probs \[4\] \[8\] \[3\] \[num_dct_tokens-1\] =",
                               1056, prose),
                       [4, 8, 3, 11], "RFC 6386 section 13.4: probabilities that a token probability is updated"))
    out.append(c_array("unsigned char", "VP8_DEFAULT_COEFF_PROBS",
                       numbers(vp8, r"const Prob default_coeff_probs \[4\] \[8\] \[3\] \[num_dct_tokens - 1\] =",
                               1056, prose),
                       [4, 8, 3, 11], "RFC 6386 section 13.5: token probabilities at the start of a key frame"))
    out.append(c_array("short", "VP8_DC_QLOOKUP",
                       numbers(vp8, r"static const int dc_qlookup\[QINDEX_RANGE\] =", 128, prose),
                       [128], "RFC 6386 section 14.1: DC dequantisation factors by index"))
    out.append(c_array("short", "VP8_AC_QLOOKUP",
                       numbers(vp8, r"static const int ac_qlookup\[QINDEX_RANGE\] =", 128, prose),
                       [128], "RFC 6386 section 14.1: AC dequantisation factors by index"))

    walked = zigzag()
    listed = numbers(vp8, r"static const unsigned int zigzag\[16\] =", 16)
    if walked != listed:
        sys.exit("the walked zigzag %s is not RFC 6386's %s" % (walked, listed))
    out.append(c_array("unsigned char", "VP8_ZIGZAG", walked, [16],
                       "the 4x4 zigzag, walked by this script; the same as RFC 6386 section 20.16 lists"))

    out.append(c_array("unsigned char", "VP8L_CODE_LENGTH_ORDER",
                       numbers(webp, r"int kCodeLengthCodeOrder\[kCodeLengthCodes\] =", 19),
                       [19], "RFC 9649 section 3.7.2.1.2: the order code length code lengths are sent in"))
    start = webp.index("pixel offset (xi, yi) is as follows:")
    end = webp.index("Figure 20: Distance Code to Neighboring Pixel Offset Mapping")
    pairs = [int(v) for v in re.findall(r"-?\d+", webp[start + len("pixel offset (xi, yi) is as follows:"):end])]
    if len(pairs) != 240:
        sys.exit("distance map: %d numbers, wanted 240" % len(pairs))
    out.append(c_array("signed char", "VP8L_DISTANCE_MAP", pairs, [120, 2],
                       "RFC 9649 section 3.6.2.2.1: distance codes 1..120 as (xi, yi), left and up"))

    head = ("/* Generated by tools/genwebptab.py from the text of RFC 6386 and RFC 9649:\n"
            "   do not edit. Each table is read from where the RFC prints it (the section\n"
            "   is given above it) and counted; the trees' names are the values of the\n"
            "   RFC's own enumerations. Nothing here is from any decoder's source: the\n"
            "   zigzag is walked by the script and only compared with RFC 6386's listing.\n"
            "     rfc6386.txt sha256 %s\n"
            "     rfc9649.txt sha256 %s */\n"
            "#pragma once\n\n" % (vp8_sum, webp_sum))
    path = os.path.join(ROOT, "userland", "webptab.h")
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write(head + "\n".join(out))
    print("wrote userland/webptab.h: %d tables" % len(out))


if __name__ == "__main__":
    main()
