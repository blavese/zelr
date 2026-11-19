"""Archives in Files: a .zip gone into as a folder, opened from, extracted.

Two archives on a fresh volume, both written by Python's zipfile: t.zip
with a deflated readme.txt, a folder notes/ with a stored a.txt and a
deflated b.txt, and an entry named ../evil.txt that would write outside
any folder it was extracted into; and bad.zip, the same with a byte of its
stored file changed. Files is driven by the keyboard and says on the
console what its status line says; what came out is read off the disk
image (tools/readfat.py).

  - Enter on t.zip goes into it: its folder and its file are listed, and
    nothing named with a dot;
  - Enter on readme.txt takes it out into /tmp, the same bytes, and opens
    it;
  - Delete inside the archive is refused: it is only read there;
  - ctrl+E takes it all out into a folder t beside it, the same bytes, and
    says the one name it would not write; nothing lands outside t;
  - Backspace leaves the archive;
  - ctrl+E on bad.zip takes out what is whole and says the rest is
    damaged, writing nothing of the damaged file.

  python tools/zipcheck.py [--keep]
"""
import io
import os
import subprocess
import sys
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "zipcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "zipcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}
WHEN = (2026, 11, 20, 12, 0, 0)

README = b"".join(b"line %d of the archive's readme\n" % i for i in range(80))
A_TXT = b"a stored file in a folder\n"
B_TXT = b"a deflated file in a folder\n" * 30


def archive(spoil=False):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data, how in (("readme.txt", README, zipfile.ZIP_DEFLATED),
                                ("notes/a.txt", A_TXT, zipfile.ZIP_STORED),
                                ("notes/b.txt", B_TXT, zipfile.ZIP_DEFLATED),
                                ("../evil.txt", b"this must not be written\n", zipfile.ZIP_STORED)):
            i = zipfile.ZipInfo(name, WHEN)
            i.compress_type = how
            z.writestr(i, data)
    data = bytearray(buf.getvalue())
    if spoil:
        at = data.index(A_TXT)
        data[at + 3] ^= 0x20
    return bytes(data)


def read(path):
    try:
        return Fat16(IMAGE).read_file(path)
    except Exception:
        return None


def wait_disk(test, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if test():
            return True
        time.sleep(0.5)
    return test()


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("archives in files")
    os.makedirs(WORK, exist_ok=True)
    files = {"t.zip": archive(), "bad.zip": archive(spoil=True)}
    for n, data in files.items():
        with open(os.path.join(WORK, n), "wb") as f:
            f.write(data)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"]
                       + [os.path.join(WORK, n) + ":home/arch/" + n for n in files],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with two archives on it", r.returncode == 0)
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
            "zip-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

        mark = len(vm.serial())
        for ch in "/bin/files /home/arch\n":
            key(NAMED.get(ch, ch), settle=0.06)
        c.add("Files opens on the folder", said_since(mark, "files: /home/arch", 30))

        # bad.zip, t.zip: the second, and into it.
        key("down"); key("down")
        mark = len(vm.serial())
        key("ret")
        inside = said_since(mark, "files: /home/arch/t.zip")
        key("down")
        folder = said_since(mark, "files: notes - folder")
        key("down")
        readme = said_since(mark, "files: readme.txt - text")
        key("down")
        time.sleep(1.0)
        out = vm.serial()[mark:]
        c.add("Enter on an archive goes into it: its folder and its file listed, no dot name",
              inside and folder and readme and "files: .. - " not in out and "evil" not in out)

        # readme.txt, opened: taken out into /tmp first.
        mark = len(vm.serial())
        key("ret")
        opened = said_since(mark, "files: opened from the archive")
        c.add("a file opened from it is taken out into /tmp, the same bytes",
              opened and wait_disk(lambda: read("/tmp/readme.txt") == README))
        time.sleep(2.0)
        key("alt-q", settle=1.0)               # the editor it opened in
        time.sleep(1.0)

        mark = len(vm.serial())
        key("delete")
        c.add("Delete inside the archive is refused: it is only read there",
              said_since(mark, "files: an archive is only read here"))

        # ctrl+E: all of it, into t beside it.
        mark = len(vm.serial())
        key("ctrl-e")
        said = said_since(mark, "files: extracted 3 files into t; not 1: a name that would write outside the folder", 30)
        whole = wait_disk(lambda: read("/home/arch/t/readme.txt") == README and read("/home/arch/t/notes/a.txt") == A_TXT
                          and read("/home/arch/t/notes/b.txt") == B_TXT)
        c.add("ctrl+E takes it all out into a folder beside it, the same bytes", said and whole)
        c.add("and writes nothing outside that folder",
              read("/home/arch/evil.txt") is None and read("/home/evil.txt") is None and read("/home/arch/t/evil.txt") is None)

        mark = len(vm.serial())
        key("backspace")
        c.add("Backspace leaves the archive", said_since(mark, "files: /home/arch\n") or said_since(mark, "files: /home/arch\r"))

        # bad.zip: second now, after the folder t (folders first).
        key("down")
        mark = len(vm.serial())
        key("down")
        c.add("bad.zip is the second entry", said_since(mark, "files: bad.zip - archive"))
        mark = len(vm.serial())
        key("ctrl-e")
        said = said_since(mark, "files: extracted 2 files into bad; not 2:", 30)
        line = vm.serial()[mark:]
        c.add("a damaged archive gives what is whole and says the rest is damaged",
              said and wait_disk(lambda: read("/home/arch/bad/readme.txt") == README)
              and read("/home/arch/bad/notes/a.txt") is None, line[-200:])
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    for n in files:
        try:
            os.remove(os.path.join(WORK, n))
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
