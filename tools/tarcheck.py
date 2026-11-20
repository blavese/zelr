"""Files reads tar, .tar.gz, .tgz and .gz as it reads zip.

Six archives on a fresh volume, all written by Python's tarfile and gzip:

  a.tar       pax: a folder, a text file, a binary file two folders down, a
              path of 110 characters in a pax record (the header's own name
              is another) and a link;
  g.tar       GNU: a path of 117 characters (a GNU long name) and another;
  d.tgz       ustar, gzipped: a path split over ustar's prefix and name;
  c.tar.gz    "./hello.txt", as tar run on "." writes it;
  n.txt.gz    one file gzipped, its name kept in the gzip;
  bad.tar.gz  c.tar.gz with the CRC-32 in its trailer changed.

mkfat writes 8.3 names only, so the three with two dots go onto the volume
as ctar.gz, ntxt.gz and badtar.gz and are renamed in the terminal first.
The kernel keeps a name to 63 bytes, so the long paths are long by depth;
d.tgz's comes out 132 bytes long, past the 127 a path could be before
0.210.0.

Files is driven by the keyboard and says on the console what its status
line says; what came out is read off the disk image.

  - ctrl+E on each takes it all out into a folder named after it, the same
    bytes under the whole names, and the link left out;
  - Enter on c.tar.gz goes into it and lists hello.txt, and Enter on that
    takes it out into /tmp and opens it;
  - Enter on n.txt.gz lists n.txt, and ctrl+E puts n.txt beside it;
  - bad.tar.gz is not opened: it is damaged, and nothing is taken out.

  python tools/tarcheck.py [--keep]
"""
import gzip
import io
import os
import random
import subprocess
import sys
import tarfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "tarcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "tarcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}

README = b"read me\nsecond line\n"
BIN = bytes(random.Random(7).randrange(256) for _ in range(3000))
LONG = "docs/" + "w" * 50 + "/" + "x" * 50 + ".txt"
GLONG = "g/" + "y" * 55 + "/" + "z" * 55 + ".txt"
SPLIT = "p/" + "q" * 60 + "/" + "r" * 55 + ".txt"
RENAMED = {"ctar.gz": "c.tar.gz", "ntxt.gz": "n.txt.gz", "badtar.gz": "bad.tar.gz"}
HELLO = b"hello from inside a tar.gz\n"
N_TXT = b"one file, gzipped\n" * 20


def tar_of(members, fmt):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=fmt) as t:
        for name, data in members:
            ti = tarfile.TarInfo(name)
            ti.mtime = 1700000000
            if name == LONG:
                # A pax record has the path; the header's own name is short.
                ti = tarfile.TarInfo("docs/short.txt")
                ti.mtime = 1700000000
                ti.pax_headers = {"path": LONG}
            if data is None:
                ti.type, ti.mode = tarfile.DIRTYPE, 0o755
                t.addfile(ti)
            elif data == "link":
                ti.type, ti.linkname = tarfile.SYMTYPE, "docs/readme.txt"
                t.addfile(ti)
            else:
                ti.size, ti.mode = len(data), 0o644
                t.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def gz_of(data, name=None):
    buf = io.BytesIO()
    with gzip.GzipFile(filename=name or "", mode="wb", fileobj=buf, mtime=0) as g:
        g.write(data)
    return buf.getvalue()


def archives():
    c = gz_of(tar_of([("./hello.txt", HELLO)], tarfile.USTAR_FORMAT))
    bad = bytearray(c)
    bad[-8] ^= 0xFF                      # the trailer's CRC-32, so only the check finds it
    return {
        "a.tar": tar_of([("docs", None), ("docs/readme.txt", README), ("docs/deep/b.bin", BIN),
                         (LONG, b"a long name, kept in a pax record\n"), ("link", "link")], tarfile.PAX_FORMAT),
        "g.tar": tar_of([(GLONG, b"a GNU long name\n"), ("g/c.txt", b"c\n")], tarfile.GNU_FORMAT),
        "d.tgz": gz_of(tar_of([("sub/d.txt", b"d\n"), (SPLIT, b"split over prefix and name\n")],
                              tarfile.USTAR_FORMAT)),
        "ctar.gz": c,
        "ntxt.gz": gz_of(N_TXT, "n.txt"),
        "badtar.gz": bytes(bad),
    }


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("tar and gzip")
    os.makedirs(WORK, exist_ok=True)
    files = archives()
    args = []
    for name, data in files.items():
        p = os.path.join(WORK, name)
        with open(p, "wb") as f:
            f.write(data)
        args.append(p + ":home/t/" + name)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "65536"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with six archives Python wrote", r.returncode == 0)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return c.report(keep=keep)

    def read(path):
        try:
            return Fat16(IMAGE).read_file(path)
        except Exception:
            return None

    vm = Guest(IMAGE, memory=256, reuse=True, keep=True)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        mon = vm.monitor()
        mon.move_to(1000, 20)
        _, _, _, shot, up = mon.wait_screen(
            "tar-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def said_since(mark, text, timeout=20):
            end = time.time() + timeout
            while time.time() < end:
                if text.lower() in vm.serial()[mark:].lower():
                    return True
                time.sleep(0.2)
            return False

        def choose(name):
            """The entry chosen, by walking down from the top until Files
            says it is the one."""
            key("end")
            mark = len(vm.serial())
            key("home")
            for _ in range(14):
                if said_since(mark, "files: %s - " % name, 1.5):
                    return True
                key("down")
            return False

        def extract(name, want):
            if not choose(name):
                return False
            mark = len(vm.serial())
            key("ctrl-e")
            return said_since(mark, "files: " + want, 30)

        mark = len(vm.serial())
        for old, new in RENAMED.items():
            vm.type("mv /home/t/%s /home/t/%s\n" % (old, new))
        vm.type("notice renamed\n")
        c.add("the names with two dots made in the terminal", said_since(mark, "notice: renamed", 30))

        mark = len(vm.serial())
        for ch in "/bin/files /home/t\n":
            key(NAMED.get(ch, ch), settle=0.06)
        c.add("Files opens on the folder", said_since(mark, "files: /home/t", 30))
        c.add("a .tgz is an archive to Files", choose("d.tgz") and said_since(mark, "files: d.tgz - archive", 2))

        c.add("ctrl+E on a.tar: 3 files into a (the link left out)", extract("a.tar", "extracted 3 files into a"))
        c.add("ctrl+E on g.tar: 2 files into g", extract("g.tar", "extracted 2 files into g"))
        c.add("ctrl+E on d.tgz: 2 files into d", extract("d.tgz", "extracted 2 files into d"))

        choose("c.tar.gz")
        mark = len(vm.serial())
        key("ret")
        inside = said_since(mark, "files: /home/t/c.tar.gz")
        key("down")
        listed = said_since(mark, "files: hello.txt - text")
        mark = len(vm.serial())
        key("ret")
        opened = said_since(mark, "files: opened from the archive")
        c.add("Enter on c.tar.gz goes into it and lists hello.txt (its ./ gone)", inside and listed)
        c.add("and hello.txt is taken out into /tmp and opened", opened)
        time.sleep(2.0)
        key("alt-q", settle=1.0)               # the editor it opened in
        time.sleep(1.0)
        mark = len(vm.serial())
        key("backspace")
        said_since(mark, "files: /home/t", 10)
        c.add("ctrl+E on c.tar.gz: 1 file into c", extract("c.tar.gz", "extracted 1 file into c"))

        choose("n.txt.gz")
        mark = len(vm.serial())
        key("ret")
        inside = said_since(mark, "files: /home/t/n.txt.gz")
        key("down")
        c.add("Enter on n.txt.gz lists the one file under the name the gzip kept: n.txt",
              inside and said_since(mark, "files: n.txt - text"))
        mark = len(vm.serial())
        key("backspace")
        said_since(mark, "files: /home/t", 10)
        c.add("ctrl+E on n.txt.gz puts n.txt beside it", extract("n.txt.gz", "extracted n.txt"))

        choose("bad.tar.gz")
        mark = len(vm.serial())
        key("ctrl-e")
        c.add("bad.tar.gz is not opened: it is damaged",
              said_since(mark, "files: not opened: it is damaged", 20))
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    want = {
        "/home/t/a/docs/readme.txt": README,
        "/home/t/a/docs/deep/b.bin": BIN,
        "/home/t/a/" + LONG: b"a long name, kept in a pax record\n",
        "/home/t/g/" + GLONG: b"a GNU long name\n",
        "/home/t/g/g/c.txt": b"c\n",
        "/home/t/d/sub/d.txt": b"d\n",
        "/home/t/d/" + SPLIT: b"split over prefix and name\n",
        "/home/t/c/hello.txt": HELLO,
        "/tmp/hello.txt": HELLO,
        "/home/t/n.txt": N_TXT,
    }
    wrong = [p for p, data in want.items() if read(p) != data]
    for p in wrong:
        print("      | %s: %r" % (p[:90], (read(p) or b"")[:60]))
    c.add("everything came out with the same bytes under its whole name (%d wrong)" % len(wrong), not wrong)
    c.add("the link was left out, and nothing came out of the damaged one",
          read("/home/t/a/link") is None and read("/home/t/bad/hello.txt") is None)

    for name in files:
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
