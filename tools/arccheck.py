"""The terminal's unzip and zip.

Archives written by Python on a fresh volume: t.zip (a deflated file, a
stored one in a folder, an empty folder, and ../evil.txt, which must not
be written), t.tgz (tarfile, gzipped) and one.gz. The commands are typed
into the desktop's terminal, each sent into a file with >, and what they
wrote is read back off the disk.

  - unzip -l lists each entry's size and name;
  - unzip t.zip takes it out into a folder beside it named t, refuses
    ../evil.txt and says so, and makes the empty folder;
  - unzip t.tgz FOLDER takes a tar.gz out into the folder named;
  - unzip one.gz takes the file out beside it, as one;
  - zip of the folder t and a file makes a zip Python's zipfile reads,
    every CRC good, the names under the folder's own, the empty folder as
    an entry, and the bytes as they went in.

  python tools/arccheck.py [--keep]
"""
import gzip
import io
import os
import subprocess
import sys
import tarfile
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "arccheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "arccheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
WHEN = (2026, 11, 20, 12, 0, 0)

A_TXT = b"".join(b"line %d of a deflated file\n" % i for i in range(200))
B_TXT = b"a stored file in a folder\n"
X_TXT = b"x from the tar\n" * 40
Z_TXT = b"z, a folder down in the tar\n"
GZ_TXT = b"hello from a gzip\n" * 9


def zip_bytes():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data, how in (("a.txt", A_TXT, zipfile.ZIP_DEFLATED),
                                ("sub/b.txt", B_TXT, zipfile.ZIP_STORED),
                                ("empty/", b"", zipfile.ZIP_STORED),
                                ("../evil.txt", b"this must not be written\n", zipfile.ZIP_STORED)):
            i = zipfile.ZipInfo(name, WHEN)
            i.compress_type = how
            z.writestr(i, data)
    return buf.getvalue()


def tgz_bytes():
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.USTAR_FORMAT) as t:
        for name, data in (("x.txt", X_TXT), ("y/z.txt", Z_TXT)):
            i = tarfile.TarInfo(name)
            i.size = len(data)
            i.mtime = 1700000000
            t.addfile(i, io.BytesIO(data))
    return buf.getvalue()


FILES = {"t.zip": zip_bytes(), "t.tgz": tgz_bytes(), "one.gz": gzip.compress(GZ_TXT, mtime=0)}
D = "/home/a/"
LINES = [
    "unzip -l %st.zip > %sl1.txt" % (D, D),
    "unzip %st.zip > %su1.txt" % (D, D),
    "unzip %st.tgz %sout > %su2.txt" % (D, D, D),
    "unzip %sone.gz > %su3.txt" % (D, D),
    "zip %sm.zip %st %sout/x.txt > %sz1.txt" % (D, D, D, D),
    "notice all done",
]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("unzip and zip")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for name, data in FILES.items():
        p = os.path.join(WORK, name)
        with open(p, "wb") as f:
            f.write(data)
        args.append(p + ":home/a/" + name)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with three archives Python wrote", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "arc-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)
        mark = len(vm.serial())
        for line in LINES:
            vm.type(line + "\n")
        end = time.time() + 120
        done = False
        while time.time() < end and not done:
            done = "notice: all done" in vm.serial()[mark:]
            time.sleep(0.2)
        c.add("the lines all ran", done)
        time.sleep(1)
    finally:
        vm.stop()

    fat = Fat16(IMAGE)

    def read(path):
        try:
            return fat.read_file(path)
        except Exception:
            return None

    def text(path):
        got = read(path)
        return None if got is None else got.decode(errors="replace")

    tree = {p.lower(): is_dir for p, _, _, is_dir, _ in fat.walk()}      # the disk may keep capitals

    l1 = text(D + "l1.txt")
    want = "".join("%9d  %s\n" % (n, name) for name, n in
                   (("a.txt", len(A_TXT)), ("sub/b.txt", len(B_TXT)), ("empty/", 0), ("../evil.txt", 25)))
    print("      | l1: %a" % l1)
    c.add("unzip -l lists each entry's size and name", l1 == want)

    u1 = text(D + "u1.txt")
    print("      | u1: %a" % u1)
    c.add("unzip t.zip says what it took out and what it refused, and why",
          u1 == "extracted 2 files into t; not 1: a name that would write outside the folder\n")
    c.add("its files are in the folder t beside it, byte for byte, and the empty folder is made",
          read(D + "t/a.txt") == A_TXT and read(D + "t/sub/b.txt") == B_TXT and tree.get(D + "t/empty") is True)
    c.add("../evil.txt was written nowhere", not any(p.endswith("/evil.txt") for p in tree))

    u2 = text(D + "u2.txt")
    print("      | u2: %a" % u2)
    c.add("unzip t.tgz FOLDER takes a tar.gz out into the folder named",
          u2 == "extracted 2 files into out\n" and read(D + "out/x.txt") == X_TXT and read(D + "out/y/z.txt") == Z_TXT)

    u3 = text(D + "u3.txt")
    print("      | u3: %a" % u3)
    c.add("unzip one.gz takes the file out beside it, as one", u3 == "extracted one\n" and read(D + "one") == GZ_TXT)

    z1 = text(D + "z1.txt") or ""
    print("      | z1: %a" % z1)
    made = read(D + "m.zip")
    names, ok, contents = [], False, {}
    if made:
        try:
            with zipfile.ZipFile(io.BytesIO(made)) as z:
                names = sorted(z.namelist())
                ok = z.testzip() is None
                contents = {n: z.read(n) for n in names if not n.endswith("/")}
        except Exception as e:
            print("      | m.zip: %a" % e)
    print("      | m.zip: %a" % names)
    c.add("zip says what it made", z1.startswith("made m.zip: 3 files, "))
    c.add("the zip it made is read by Python's zipfile, every CRC good, with the folder's names and the empty folder",
          ok and names == sorted(["t/", "t/a.txt", "t/sub/", "t/sub/b.txt", "t/empty/", "x.txt"]))
    c.add("and its files are the bytes that went in",
          contents == {"t/a.txt": A_TXT, "t/sub/b.txt": B_TXT, "x.txt": X_TXT})

    for name in FILES:
        try:
            os.remove(os.path.join(WORK, name))
        except OSError:
            pass
    try:
        os.rmdir(WORK)
    except OSError:
        pass
    if not keep:
        try:
            os.remove(IMAGE)
        except OSError:
            pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
