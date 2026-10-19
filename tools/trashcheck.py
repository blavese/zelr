"""Files' trash: deleted things kept in /home/trash until it is emptied.

Delete in Files used to take a thing off the disk at once. It puts it in
/home/trash now, with where it came from in /home/trash/.where, and ctrl+z
there puts it back; in the trash Delete deletes for good on a second press,
and ctrl+delete twice empties it. Files is driven by the keyboard and says
on the console what its status line says ("files: moved to the trash");
what happened is read back off the disk image here (tools/readfat.py), as
filescheck does, waiting on the disk rather than for a time.

On a volume with two files of the same name in two folders and a folder
with a file in it:

  - Delete puts a file in the trash, the same bytes, and notes where it was;
  - the second of the same name is kept beside it as "a.txt 2";
  - a folder goes in whole;
  - ctrl+z in the trash puts "a.txt 2" back where it came from, the other
    folder, and the note goes;
  - Delete on a thing in the trash asks first, and the second press deletes
    it for good;
  - ctrl+delete asks, and twice empties the trash.

  python tools/trashcheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "trash.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "trash.%d" % os.getpid())
WINDOW = (0xFA, 0xFA, 0xFC)            # Files' list (filescheck)
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}


def read(path):
    try:
        return Fat16(IMAGE).read_file(path)
    except Exception:          # gone, or a directory caught half written
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
    c = Checks("files' trash")
    os.makedirs(WORK, exist_ok=True)
    one = b"the first a.txt, in docs\n" * 40
    two = b"the second a.txt, in misc\n" * 30
    inner = b"a file in a folder\n"
    files = {"one": one, "two": two, "inner": inner}
    for n, data in files.items():
        with open(os.path.join(WORK, n), "wb") as f:
            f.write(data)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768",
                        os.path.join(WORK, "one") + ":home/docs/a.txt",
                        os.path.join(WORK, "two") + ":home/misc/a.txt",
                        os.path.join(WORK, "inner") + ":home/docs/sub/inner.txt"],
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with two a.txt in two folders, and a folder", r.returncode == 0)
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
            "trash-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
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

        def files_on(path):
            """Files opened on a folder from the terminal, the keyboard its."""
            mark = len(vm.serial())
            for ch in "/bin/files %s\n" % path:
                key(NAMED.get(ch, ch), settle=0.06)
            return said_since(mark, "files: %s" % path, 30)

        def close():
            key("alt-q", settle=1.0)

        # docs: sub, a.txt (folders first).
        files_on("/home/docs")
        key("down"); key("down")           # a.txt
        mark = len(vm.serial())
        key("delete")
        moved = wait_disk(lambda: read("/home/trash/a.txt") == one) and read("/home/docs/a.txt") is None
        c.add("delete puts a file in the trash, the same bytes", moved and said_since(mark, "files: moved to the trash"))
        where = (read("/home/trash/.where") or b"").decode("utf-8", "replace")
        c.add("and notes where it was", "a.txt\t/home/docs/a.txt" in where)
        key("down")                        # sub, the list read again
        key("delete")
        c.add("a folder goes in whole",
              wait_disk(lambda: read("/home/trash/sub/inner.txt") == inner) and read("/home/docs/sub/inner.txt") is None)
        close()

        # misc's a.txt, the same name: kept beside the first.
        files_on("/home/misc")
        key("down")
        key("delete")
        c.add("a second of the same name is kept beside it, as a.txt 2",
              wait_disk(lambda: read("/home/trash/a.txt 2") == two) and read("/home/trash/a.txt") == one)
        close()

        # In the trash: sub, a.txt, a.txt 2.
        files_on("/home/trash")
        key("down"); key("down"); key("down")      # a.txt 2
        mark = len(vm.serial())
        key("ctrl-z")
        back = wait_disk(lambda: read("/home/misc/a.txt") == two) and read("/home/trash/a.txt 2") is None
        c.add("ctrl+z puts it back where it came from", back and said_since(mark, "files: put back in /home/misc"))
        where = (read("/home/trash/.where") or b"").decode("utf-8", "replace")
        c.add("and its note goes, the others stay",
              "a.txt 2\t" not in where and "a.txt\t/home/docs/a.txt" in where and "sub\t/home/docs/sub" in where)

        # Delete in the trash: once asks, twice deletes for good. sub, a.txt.
        key("down")                        # sub
        mark = len(vm.serial())
        key("delete")
        asked = said_since(mark, "files: press delete again to delete it for good")
        time.sleep(1.0)
        c.add("delete in the trash asks first", asked and read("/home/trash/sub/inner.txt") == inner)
        key("delete")
        c.add("and the second press deletes it for good",
              wait_disk(lambda: read("/home/trash/sub/inner.txt") is None))

        # ctrl+delete: asks, then empties.
        mark = len(vm.serial())
        key("ctrl-delete")
        asked = said_since(mark, "files: press ctrl+delete again to empty the trash")
        time.sleep(1.0)
        c.add("ctrl+delete asks before it empties the trash", asked and read("/home/trash/a.txt") == one)
        key("ctrl-delete")
        c.add("and the second empties it",
              wait_disk(lambda: read("/home/trash/a.txt") is None) and said_since(mark, "files: the trash is empty"))
        close()
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
