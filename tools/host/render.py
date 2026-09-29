"""Pages rendered by the host build of the browser, many at once.

  python tools/host/render.py OUTDIR [-s SCRIPT] [-j JOBS] [-t SECONDS]
                              [--sheet COLS] name=address [name=address ...]

Each page gives OUTDIR/name0.png (one per `shot` in the script, default
"shot"), OUTDIR/name.txt (everything the browser printed) and its console
line here. --sheet tiles the first picture of each into OUTDIR/sheet.png at
half size, for looking at a dozen sites in one go.

Needs build/host/browser.exe (bash tools/host/build.sh) and the TLS proxy
running (python tools/host/tlsproxy.py &). A host tool: see
tools/host/shim.c for what it is and is not.
"""
import concurrent.futures as cf
import os
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.path.join(ROOT, "build", "host", "browser.exe")


def png_write(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)


def ppm_read(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def one(outdir, name, url, script, secs):
    env = dict(os.environ, HOST_SHOT=os.path.join(outdir, name), HOST_SCRIPT=script,
               HOST_SECS=str(secs))
    try:
        p = subprocess.run([EXE, url], env=env, capture_output=True, timeout=secs + 30)
        out, code = p.stdout.decode("utf-8", "replace"), p.returncode
    except subprocess.TimeoutExpired as e:
        out, code = (e.stdout or b"").decode("utf-8", "replace") + "\nHOST: killed", -9
    for f in sorted(os.listdir(outdir)):
        if f.startswith(name) and f.endswith(".ppm"):
            w, h, rgb = ppm_read(os.path.join(outdir, f))
            png_write(os.path.join(outdir, f[:-4] + ".png"), w, h, rgb)
            os.remove(os.path.join(outdir, f))
    open(os.path.join(outdir, name + ".txt"), "w", encoding="utf-8").write(out)
    lines = [l for l in out.splitlines() if l.startswith("browser: ") or l.startswith("HOST")]
    return name, code, lines


def sheet(outdir, names, cols):
    imgs = []
    for n in names:
        p = os.path.join(outdir, n + "0.png")
        if not os.path.exists(p):
            continue
        data = open(p, "rb").read()
        w, h = struct.unpack(">II", data[16:24])
        pos, idat = 8, b""
        while pos < len(data):
            ln = struct.unpack(">I", data[pos:pos + 4])[0]
            if data[pos + 4:pos + 8] == b"IDAT":
                idat += data[pos + 8:pos + 8 + ln]
            pos += 12 + ln
        raw = zlib.decompress(idat)
        imgs.append((w, h, [raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)] for y in range(h)]))
    if not imgs:
        return
    cw, ch = max(i[0] for i in imgs) // 2, max(i[1] for i in imgs) // 2
    rows = (len(imgs) + cols - 1) // cols
    W, H = cw * cols + 4 * (cols - 1), ch * rows + 4 * (rows - 1)
    out = bytearray(b"\x40" * (W * H * 3))
    for k, (w, h, lines) in enumerate(imgs):
        ox, oy = (k % cols) * (cw + 4), (k // cols) * (ch + 4)
        for y in range(h // 2):
            src = lines[y * 2]
            row = bytearray()
            for x in range(w // 2):
                row += src[x * 6:x * 6 + 3]
            at = ((oy + y) * W + ox) * 3
            out[at:at + len(row)] = row
    png_write(os.path.join(outdir, "sheet.png"), W, H, bytes(out))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    outdir = args.pop(0)
    script, jobs, secs, cols = "shot", 6, 90, 0
    while args and args[0].startswith("-"):
        k = args.pop(0)
        if k == "-s":
            script = args.pop(0)
        elif k == "-j":
            jobs = int(args.pop(0))
        elif k == "-t":
            secs = int(args.pop(0))
        elif k == "--sheet":
            cols = int(args.pop(0))
    os.makedirs(outdir, exist_ok=True)
    work = [a.split("=", 1) for a in args]
    with cf.ThreadPoolExecutor(jobs) as ex:
        for name, code, lines in ex.map(lambda nu: one(outdir, nu[0], nu[1], script, secs), work):
            print("%-10s rc=%s" % (name, code))
            for l in lines:
                print("    " + l[:300])
    if cols:
        sheet(outdir, [n for n, _ in work], cols)
    return 0


if __name__ == "__main__":
    sys.exit(main())
