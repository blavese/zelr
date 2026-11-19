"""Looking for a file in Files: every folder below, by part of its name.

Files on a fresh volume, some deep in folders; Files driven by its keys,
saying on the console what its status line says ("files: found 3 below
/home", and each result chosen as "PATH - kind"); what Delete did read
back off the disk image (tools/readfat.py).

  - "/" puts the keyboard in the filter, and Return there finds every name
    with "report" in it below /home, folders deep, and nothing else;
  - the results are listed by their paths from /home;
  - Delete on a result puts that file in the trash, found by its own
    path: this one's path from /home is longer than an entry's name holds;
  - a word nothing has in its name is said to match nothing;
  - Escape puts the folder back;
  - Return on a folder among the results goes into it.

  python tools/searchcheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "searchcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "searchcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
NAMED = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}

# mkfat writes 8.3 names, so the names are short. The long one's path from
# /home is 64 characters, one more than an entry's name keeps.
LONG = "longer01/longer02/longer03/longer04/longer05/longer06/report.txt"
FILES = {
    "docs/y2026/report.txt": b"this year's report\n",
    "docs/oldrep.txt": b"not a match: no 'report' in the name\n",
    "docs/report1.txt": b"a report in docs\n",
    "misc/deep/a/report.md": b"# a report, four folders down\n",
    "misc/notes.txt": b"nothing here\n",
    LONG: b"a report six folders down, its path longer than a name\n",
}


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
    c = Checks("looking for a file in files")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for i, (path, data) in enumerate(FILES.items()):
        src = os.path.join(WORK, "f%d" % i)
        with open(src, "wb") as f:
            f.write(data)
        args.append(src + ":home/" + path)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with files in folders", r.returncode == 0)
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
            "search-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)

        def key(name, settle=0.3):
            mon.send("sendkey %s" % name, settle=settle)

        def keys(text):
            for ch in text:
                key(NAMED.get(ch, ch), settle=0.06)

        def said_since(mark, text, timeout=20):
            end = time.time() + timeout
            while time.time() < end:
                if text in vm.serial()[mark:]:
                    return True
                time.sleep(0.2)
            return False

        mark = len(vm.serial())
        keys("/bin/files /home\n")
        c.add("Files opens on /home", said_since(mark, "files: /home", 30))

        mark = len(vm.serial())
        key("slash")
        keys("report")
        key("ret")
        found = said_since(mark, "files: found 4 below /home")
        c.add("/ and Return look in every folder below for the name, and find the four", found)

        mark = len(vm.serial())
        for _ in range(4):
            key("down")
        time.sleep(0.5)
        out = vm.serial()[mark:]
        listed = all(("files: %s - " % p) in out for p in ("docs/report1.txt", "docs/y2026/report.txt",
                                                          "misc/deep/a/report.md"))
        listed = listed and ("files: %s" % LONG[:60]) in out
        if not (listed and "oldrep" not in out):
            print("      | " + " / ".join(l.strip() for l in out.splitlines() if l.startswith("files: ")))
        c.add("listed by their paths from /home, each with its kind", listed and "oldrep" not in out)

        # Up to the third, the long one: Delete puts it in the trash, by its
        # whole path rather than by the name shown, which is cut short.
        key("up")
        mark = len(vm.serial())
        key("delete")
        c.add("Delete on a result puts that file in the trash",
              said_since(mark, "files: moved to the trash")
              and wait_disk(lambda: read("/home/trash/report.txt") == FILES[LONG])
              and read("/home/" + LONG) is None
              and read("/home/misc/deep/a/report.md") == FILES["misc/deep/a/report.md"])

        mark = len(vm.serial())
        key("slash")
        for _ in range(6):
            key("backspace", settle=0.05)
        keys("zebra")
        key("ret")
        c.add("a word nothing has is said to match nothing",
              said_since(mark, "files: nothing below /home has zebra in its name"))

        mark = len(vm.serial())
        key("slash")
        key("esc")
        c.add("Escape puts the folder back", said_since(mark, "files: /home"))

        # docs/y2026 is a folder with "2026" in its name.
        mark = len(vm.serial())
        key("slash")
        keys("2026")
        key("ret")
        said_since(mark, "files: found 1 below /home")
        key("down")
        key("ret")
        c.add("Return on a folder among the results goes into it", said_since(mark, "files: /home/docs/y2026"))
        key("alt-q", settle=1.0)
    finally:
        vm.stop()

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
