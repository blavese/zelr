"""Files makes a .zip, and another system reads it.

A folder of files on a fresh volume, some text and some noise, and a file
beside it. Files is driven by its keys and says on the console what its
status line says. The archive it writes is read back off the disk image
(tools/readfat.py) and opened with Python's zipfile, which is the other
system: a writer checked only against its own reader agrees with itself.

  - ctrl+K on a folder makes NAME.zip of it: everything below it, by its
    path, folders as entries of their own;
  - zipfile opens it, finds every CRC right and every byte as it was;
  - the text in it is deflated to less than half, the noise kept as it is;
  - on a file it makes the file's name with .zip for its extension;
  - a name already taken gets " 2";
  - and Files goes into the archive it made.

  python tools/zipmakecheck.py [--keep]
"""
import io
import os
import random
import subprocess
import sys
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "zipmakecheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "zipmakecheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}

rng = random.Random(20261120)
WORDS = b"the machine makes an archive of its own files and another system reads it".split()
TEXT = b" ".join(rng.choice(WORDS) for _ in range(6000)) + b"\n"
NOISE = bytes(rng.randrange(256) for _ in range(8000))
FILES = {
    "proj/a.txt": TEXT,
    "proj/sub/b.bin": NOISE,
    "proj/sub/c.txt": b"a small file, deeper down\n",
    "one.txt": b"one file of its own\n" * 40,
}


def unpacked(z, name):
    """A file out of the archive, or None: zipfile raises on a bad CRC or
    deflate data that does not decode, and that is a failed check, not a
    harness that stopped."""
    try:
        return z.read(name)
    except Exception as e:                     # noqa: BLE001
        print("      | %s: %s" % (name, e))
        return None


def read(path):
    try:
        return Fat16(IMAGE).read_file(path)
    except Exception:
        return None


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("files makes a zip")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for i, (path, data) in enumerate(FILES.items()):
        src = os.path.join(WORK, "f%d" % i)
        with open(src, "wb") as f:
            f.write(data)
        args.append(src + ":home/mk/" + path)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with a folder of files and a file beside it", r.returncode == 0)
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
            "zipmake-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def keys(text):
            for ch in text:
                key(NAMED.get(ch, ch), settle=0.06)

        def said_since(mark, text, timeout=30):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        mark = len(vm.serial())
        keys("/bin/files /home/mk\n")
        c.add("Files opens on the folder", said_since(mark, "files: /home/mk"))

        mark = len(vm.serial())
        key("down")
        said_since(mark, "files: proj - folder", 10)
        mark = len(vm.serial())
        key("ctrl-k")
        c.add("ctrl+K on the folder makes proj.zip of its three files",
              said_since(mark, "files: made proj.zip: 3 files, "))

        # The list is proj, one.txt, proj.zip, and proj.zip is chosen.
        mark = len(vm.serial())
        key("up")
        said_since(mark, "files: one.txt - text", 10)
        mark = len(vm.serial())
        key("ctrl-k")
        c.add("and on a file, the file's name with .zip for its extension",
              said_since(mark, "files: made one.zip: 1 file, "))

        mark = len(vm.serial())
        key("home")
        said_since(mark, "files: proj - folder", 10)
        mark = len(vm.serial())
        key("ctrl-k")
        c.add("a name already taken gets a 2", said_since(mark, "files: made proj 2.zip: 3 files, "))

        # Into the archive it made: proj, one.txt, one.zip, proj 2.zip, proj.zip.
        mark = len(vm.serial())
        key("end")
        said_since(mark, "files: proj.zip - archive", 10)
        mark = len(vm.serial())
        key("ret")
        inside = said_since(mark, "files: /home/mk/proj.zip")
        key("down")
        c.add("and Files goes into the archive it made", inside and said_since(mark, "files: proj - folder", 10))
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

    # --- what another system makes of it -------------------------------------
    raw = read("/home/mk/proj.zip")
    z = None
    try:
        z = zipfile.ZipFile(io.BytesIO(raw)) if raw else None
    except zipfile.BadZipFile as e:
        print("      | zipfile: %s" % e)
    c.add("Python's zipfile opens it", z is not None)
    if z:
        names = sorted(z.namelist())
        want = ["proj/", "proj/a.txt", "proj/sub/", "proj/sub/b.bin", "proj/sub/c.txt"]
        if names != want:
            print("      | names: %s" % names)
        c.add("with everything below the folder, by its path, the folders too", names == want)
        try:
            bad = z.testzip()
        except Exception as e:                 # noqa: BLE001
            bad = str(e)
        c.add("every CRC right", bad is None)
        same = all(unpacked(z, p) == FILES[p] for p in FILES if p.startswith("proj/"))
        c.add("every byte as it was", same)
        a, b = z.getinfo("proj/a.txt"), z.getinfo("proj/sub/b.bin")
        c.add("the text deflated to less than half (%d of %d bytes)" % (a.compress_size, a.file_size),
              a.compress_type == zipfile.ZIP_DEFLATED and a.compress_size * 2 < a.file_size)
        c.add("the noise kept as it was", b.compress_type == zipfile.ZIP_STORED and b.compress_size == len(NOISE))
    one = read("/home/mk/one.zip")
    try:
        oz = zipfile.ZipFile(io.BytesIO(one)) if one else None
        c.add("one.zip holds one.txt as it was", oz is not None and oz.namelist() == ["one.txt"]
              and unpacked(oz, "one.txt") == FILES["one.txt"])
    except zipfile.BadZipFile:
        c.add("one.zip holds one.txt as it was", False)

    for i in range(len(FILES)):
        try:
            os.remove(os.path.join(WORK, "f%d" % i))
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
