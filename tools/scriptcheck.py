"""The terminal's scripts: source, $1 to $9, and set's names.

Two scripts on a fresh volume. make.sh sets a name and writes files from
its words; loop.sh adds a line to a file and then sources itself, which
must be refused rather than go round for ever. They are run by lines typed
into the desktop's terminal, and what they wrote is read back off the disk.

  - $NAME from set, $1 and $2 (also inside double quotes), $# and $*, $0;
  - a comment and an empty line passed over (a comment run as a line would
    say a fault on the screen, so the fault's colour is counted there);
  - a script sourcing itself runs once;
  - set alone lists the names, and a typed line has its $NAME put in;
  - set NAME alone takes the name away.

  python tools/scriptcheck.py [--keep]
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, count_in, count_near, ROOT   # noqa: E402
from readfat import Fat16                                    # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
IMAGE = os.path.join(ROOT, "scriptcheck.%d.img" % os.getpid())
WORK = os.path.join(ROOT, "scriptcheck.%d" % os.getpid())
TERMINAL = (0x10, 0x14, 0x1A)
SCREEN = (0, 0, 1024, 768)
D = "/home/s/"

MAKE = "\n".join([
    "# a comment, passed over",
    "set greeting hello",
    "",
    "echo $greeting $1 > %so1.txt" % D,
    'echo "$2 and $1" > %so2.txt' % D,
    "echo $# words: $* > %so3.txt" % D,
    "mkdir %s$1" % D,
    "write %s$1/f.txt from $0" % D,
    "  # an indented comment",
    "echo a $ alone, and $nothing set > %so8.txt" % D,
    "notice made",
]) + "\n"
# What the terminal writes a fault in. A comment run as a line says "#: not
# a command" in it and nothing else, so the screen is where it shows.
WARN = (0xE0, 0x7A, 0x6A)
LOOP = "append %so5.txt once\nsource %sloop.sh\n" % (D, D)
FILES = {"make.sh": MAKE, "loop.sh": LOOP}
LINES = [
    "source %smake.sh alpha beta" % D,
    "source %sloop.sh" % D,
    "set > %so6.txt" % D,
    "echo $greeting typed > %so7.txt" % D,
    "set greeting",
    "echo [$greeting] > %so9.txt" % D,
    "notice all done",
]


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("scripts")
    os.makedirs(WORK, exist_ok=True)
    args = []
    for name, text in FILES.items():
        p = os.path.join(WORK, name)
        with open(p, "w", newline="\n") as f:
            f.write(text)
        args.append(p + ":home/s/" + name)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"), IMAGE, "32768"] + args,
                       cwd=ROOT, capture_output=True, text=True)
    c.add("a volume with two scripts", r.returncode == 0)
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
            "script-desk", lambda w, h, px: count_in(px, w, SCREEN, TERMINAL) > 50000, timeout=60)
        c.add("the desktop comes up with its terminal", up, shot)
        time.sleep(1)
        w, h, px, _ = mon.screen("script-before")
        warned_before = count_near(px, w, SCREEN, WARN, 24)
        mark = len(vm.serial())
        vm.type(LINES[0] + "\n")
        end = time.time() + 60
        while time.time() < end and "notice: made" not in vm.serial()[mark:]:
            time.sleep(0.2)
        time.sleep(1)
        w, h, px, shot = mon.screen("script-made")
        warned = count_near(px, w, SCREEN, WARN, 24) - warned_before
        print("      | the warning's colour %d pixels more after make.sh" % warned)
        c.add("make.sh runs with nothing said wrong: its comments and empty line passed over", warned < 40, shot)
        for line in LINES[1:]:
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

    def read(name):
        try:
            got = Fat16(IMAGE).read_file(D + name)
            return None if got is None else got.decode(errors="replace")
        except Exception:
            return None

    def same(label, name, want):
        got = read(name)
        if got != want:
            print("      | %s: got %a, want %a" % (name, got, want))
        c.add(label, got == want)

    same("$NAME from set and $1", "o1.txt", "hello alpha\n")
    same("$2 and $1 inside double quotes", "o2.txt", "beta and alpha\n")
    same("$# and $*", "o3.txt", "2 words: alpha beta\n")
    same("a folder named by $1, and a file written there from $0", "alpha/f.txt", "from %smake.sh\n" % D)
    same("a $ that names nothing stays, and a name never set is nothing", "o8.txt", "a $ alone, and set\n")
    same("a script that sources itself runs once", "o5.txt", "once\n")
    same("set alone lists the names", "o6.txt", "greeting=hello\n")
    same("a typed line has its $NAME put in", "o7.txt", "hello typed\n")
    same("set NAME alone takes it away", "o9.txt", "[]\n")

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
