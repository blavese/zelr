"""What the harnesses that drive a running machine all need.

Three of them boot zelr, press keys at it and look at the screen, and all
three used to wait by sleeping: type something, sleep three seconds, take a
picture, decide. That works on an idle machine and stops working the moment
the gate runs four of them at once, because the thing being waited for now
takes four seconds. The failure that produces is the worst kind: it names a
feature, it moves around between runs, and it sends whoever reads it looking
for a bug in the kernel that is not there. A whole day went into chasing
three of those.

So nothing here waits for a length of time. It waits for the thing it is
waiting for, and gives up only when waiting has clearly stopped being
useful. A check that passes in 300 ms passes in 300 ms; one that needs eight
seconds under load gets eight seconds; one that is genuinely broken is told
apart from one that is merely slow by whether it ever happens at all.

The pieces:

  Guest     a booted machine, with its serial output drained as it arrives
            so nothing blocks, and waits that watch for text rather than
            counting seconds
  Monitor   QEMU's monitor, framed on its prompt, so a command is finished
            when QEMU says it is finished
  counting  finding a colour fast enough to be worth doing repeatedly
"""
import os
import re
import socket
import subprocess
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")


def qemu_path():
    q = os.environ.get("QEMU") or "C:/Program Files/qemu/qemu-system-x86_64.exe"
    if not os.path.exists(q):
        from shutil import which
        q = which("qemu-system-x86_64") or q
    return q


def free_port():
    """A port nothing else is on.

    These used to be constants, one per harness, which is fine until two
    runs overlap. They did overlap, more than once, and the second machine
    then drove the first one's monitor: every picture it took was of the
    other machine's screen, and the checks failed describing windows that
    had never been asked for."""
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Timeout(Exception):
    """Raised when something waited for never happened."""


# The shell's prompt, which carries the working directory: `zelr> ` at the
# root and `zelr:/home> ` everywhere else, and it starts in /home.
PROMPT = re.compile(r"zelr(?::\S*)?> ")

# How long to leave between one pointer movement and the next. The PS/2
# queue is short and the guest has to be given time to empty it.
MOUSE_GAP = 0.05

# The two colours the window manager draws its pointer in, and where the
# first pixel of the lighter one sits relative to the point the pointer is
# actually at. From the glyph in wm.c: its first two rows are all outline,
# and the fill starts one across on the third.
CURSOR_FILL = (0xF4, 0xF7, 0xF9)
CURSOR_EDGE = (0x08, 0x0C, 0x10)
CURSOR_FILL_AT = (1, 2)
# The glyph carries about seventy pixels of fill and forty of outline. These
# are low enough that most of it can be off the edge of the screen and it is
# still found, and high enough that a stray pixel of either colour is not
# mistaken for it.
CURSOR_FILL_MIN = 20
CURSOR_EDGE_MIN = 10

# The most a single pointer movement may carry. A PS/2 packet has eight
# signed bits per axis and a flag for when that was not enough, and the
# driver throws away a packet with that flag set, so anything past this is
# not a large movement but a missing one.
MOUSE_STEP = 100


# --- counting colours ------------------------------------------------------
#
# Every check in these harnesses ends in "how much of this colour is in this
# part of the screen", and each one used to walk the pixels in Python: three
# quarters of a million slice-and-compares, well over a second each time.
# That was survivable when a picture was taken once per check. It is not
# survivable now that waiting means taking one every quarter second.
#
# So the searching is left to bytes.find, which is C, and Python only sees
# the matches. A pixel is three bytes, so a hit has to sit on a multiple of
# three to be a pixel rather than the tail of one and the head of the next.

def _count(buf, want, base=0):
    n = 0
    start = 0
    find = buf.find
    while True:
        i = find(want, start)
        if i < 0:
            return n
        if (i + base) % 3 == 0:
            n += 1
        start = i + 1


def count_all(px, rgb):
    """How many pixels of this colour are anywhere on the screen."""
    return _count(px, bytes(rgb))


def count_in(px, w, rect, rgb):
    """How many pixels of this colour are inside this rectangle."""
    left, top, right, bottom = rect
    want = bytes(rgb)
    n = 0
    for y in range(top, bottom):
        row = y * w * 3
        n += _count(px[row + left * 3:row + right * 3], want)
    return n


def count_near(px, w, rect, rgb, tol=8):
    """How many pixels inside this rectangle are within `tol` of this colour.

    For surfaces that do not have one colour. A pane that is tinted glass
    over a wallpaper takes a little of whatever is behind it, so it comes out
    a shade different in one place from another and an exact count of it
    finds almost nothing. The tolerance is what turns "this colour" into
    "this surface", and it wants to be smaller than the step between the
    surfaces being told apart.
    """
    left, top, right, bottom = rect
    r, g, b = rgb
    n = 0
    for y in range(top, bottom):
        row = y * w * 3
        for x in range(left, right):
            o = row + x * 3
            if (abs(px[o] - r) <= tol and abs(px[o + 1] - g) <= tol
                    and abs(px[o + 2] - b) <= tol):
                n += 1
    return n


def row_mean(px, w, y, left, right):
    """The average colour of one row, between two columns."""
    r = g = b = 0
    row = y * w * 3
    for x in range(left, right):
        o = row + x * 3
        r += px[o]
        g += px[o + 1]
        b += px[o + 2]
    n = max(1, right - left)
    return (r // n, g // n, b // n)


def colour_gap(a, b):
    """How far apart two colours are, added up across the channels."""
    return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])


def centre_of(px, w, h, rgb, min_pixels=200, within=None):
    """Where a block of one colour is, as (x, y) of its middle.

    Clicking a remembered coordinate is how these harnesses kept breaking: a
    swatch moves because a window gained a sidebar, and the click lands on
    whatever is there now. So the thing is found in the picture instead.
    None when it is not on screen, which is an answer and not a coordinate
    to click anyway.

    within limits it to a rectangle. The taskbar draws every pinned app in a
    colour from the same palette the themes use, so a colour that used to
    mean one thing on screen now means that thing or an icon, and the answer
    to "where is it" was the point between the two."""
    want = bytes(rgb)
    xs = ys = n = 0
    start = 0
    while True:
        i = px.find(want, start)
        if i < 0:
            break
        start = i + 1
        if i % 3:
            continue
        y, x = divmod(i // 3, w)
        if within and not (within[0] <= x < within[2]
                           and within[1] <= y < within[3]):
            continue
        xs += x
        ys += y
        n += 1
    if n < min_pixels:
        return None
    return (xs // n, ys // n)


# --- how wide a piece of chrome text is ------------------------------------
#
# Several checks need to know where something at the right hand end of the
# panel sits, and where it sits is measured in from the edge past the clock.
# That was written down as a number, and the number was the width of five
# digits in the fifteen pixel face; the clock was then set in the twenty
# pixel one and every position derived from it moved by thirteen pixels
# while the constant went on saying thirty six.
#
# The advances are in the generated header, so they can be read rather than
# remembered. This is the same sum face_width does in gfx.c.
FACE_BODY, FACE_HEAD, FACE_TITLE, FACE_BODY_BOLD, FACE_HEAD_BOLD = 1, 2, 3, 4, 5

_ADVANCES = {}


def face_width(text, which=FACE_BODY):
    """The width of a string in one of the kernel's faces, in pixels."""
    if not _ADVANCES:
        head = os.path.join(ROOT, "include", "face.h")
        with open(head, encoding="utf-8") as fh:
            src = fh.read()
        # The tables in the order face_faces lists them, which is the order
        # the FACE_ numbers above index.
        order = ["face_g_13", "face_g_15", "face_g_20", "face_g_26",
                 "face_g_15b", "face_g_20b", "face_g_15m", "face_g_15bm"]
        for i, name in enumerate(order):
            at = src.index("static const face_glyph %s[95] = {" % name)
            end = src.index("\n};", at)
            rows = re.findall(r"\{([^{}]*)\}", src[at:end])
            adv = []
            for row in rows:
                parts = [p.strip() for p in row.split(",")]
                adv.append(int(parts[4]))
            assert len(adv) == 95, "%s has %d glyphs" % (name, len(adv))
            _ADVANCES[i] = adv

    adv = _ADVANCES[which]
    total = 0
    for ch in text:
        c = ord(ch)
        if c < 32 or c > 126:
            c = 32
        total += adv[c - 32]
    return total


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 ppm")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h, _maxval = fields
    px = data[pos:pos + w * h * 3]
    if len(px) != w * h * 3:
        raise ValueError("the picture is %d bytes short"
                         % (w * h * 3 - len(px)))
    return w, h, px


# --- the monitor -----------------------------------------------------------

class Monitor:
    """QEMU's text monitor over TCP: the mouse, the keyboard and the screen,
    none of which the guest can be asked to provide for us.

    Every command is finished when QEMU prints its prompt again. That sounds
    obvious and it is the whole point: this used to send a command, sleep a
    fixed time and read whatever had arrived. A command that took longer
    than the sleep left its reply in the socket, so the next command read
    the previous one's output, and everything after that was one behind."""

    PROMPT = b"(qemu)"

    def __init__(self, port, timeout=180):
        end = time.time() + 20
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port),
                                                  timeout=5)
                break
            except OSError:
                if time.time() >= end:
                    raise
                time.sleep(0.2)
        self.s.settimeout(timeout)
        self.buf = b""

        self._to_prompt()          # the banner it opens with

    def _to_prompt(self, timeout=180):
        end = time.time() + timeout
        while self.PROMPT not in self.buf:
            if time.time() >= end:
                raise Timeout("the monitor stopped answering")
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                raise Timeout("the monitor stopped answering")
            if not chunk:
                raise Timeout("the monitor closed")
            self.buf += chunk
        out, _, rest = self.buf.partition(self.PROMPT)
        self.buf = rest
        return out.decode("utf-8", "replace")

    def send(self, command, settle=0.0):
        self.s.sendall((command + "\n").encode())
        out = self._to_prompt()
        # Some things need a moment in the guest after the monitor is done
        # with them: a key press has to be picked up by a program, and the
        # monitor knows nothing about that.
        if settle:
            time.sleep(settle)
        return out

    # --- the pointer -------------------------------------------------------
    def move_to(self, x, y):
        """The PS/2 pointer is relative and one large jump gets clamped, so
        walk it into the corner and step out from there.

        The step out has to be split as well. Measured: a single move to
        x=985 left the pointer at 635, which is how a click aimed at a
        button on the right of the screen quietly lands in the middle of a
        title bar instead. Nothing reports this; the click simply does
        something else.

        The gap after each step is not politeness. The guest reads these
        through a PS/2 port with a small queue, and sending the whole walk
        as fast as the monitor accepts it loses some of the middle: the
        pointer ends up short of where it was sent, and again the click
        lands somewhere else without anything saying so. This was measured
        the hard way, by taking the gaps out.

        Measured, with the gaps in, this lands exactly on target every time,
        including the far right of the screen where the window buttons are.
        An attempt to correct it afterwards by reading the pointer off the
        screen made things worse rather than better, because the screen lags
        the mouse: the reading taken straight after a move is the position
        before it, and correcting from that moves the pointer somewhere it
        was never sent. What was actually going wrong is in click."""
        for _ in range(12):
            self.send("mouse_move -200 -200", settle=MOUSE_GAP)
        at_x = at_y = 0
        while at_x < x or at_y < y:
            dx = min(MOUSE_STEP, x - at_x)
            dy = min(MOUSE_STEP, y - at_y)
            self.send("mouse_move %d %d" % (dx, dy), settle=MOUSE_GAP)
            at_x += dx
            at_y += dy
        time.sleep(0.2)

    def click(self, x, y):
        self.move_to(x, y)

        # And then check it got there, walking again if it did not.
        #
        # The walk is reliable until the machine is busy. With several
        # windows up and the compositor working, some of the steps are lost
        # and the pointer ends up short: measured, a walk to the launcher
        # from the panel did not move it at all, and the click went to the
        # icon it was already sitting on.
        #
        # Reading the pointer straight after a move gives the position
        # before it, which is why correcting from one reading made this
        # worse when it was tried before. `pointer` waits for two readings
        # that agree, which is where it has come to rest, and a second walk
        # from a known position lands.
        for _ in range(2):
            at = self.pointer()
            if at is None:
                break
            if abs(at[0] - x) <= 3 and abs(at[1] - y) <= 3:
                break
            self.move_to(x, y)

        # Held, rather than pressed and let go as fast as the monitor will
        # take it. The window manager reads the mouse once per pass of its
        # own loop, and a pass that ends in compositing the whole screen is
        # not short. A press and release that both happen inside one of them
        # is a click it never sees: the button state it reads is the state
        # before, and then the state after, and nothing in between ever
        # happened as far as it is concerned.
        self.send("mouse_button 1", settle=0.4)
        self.send("mouse_button 0", settle=0.5)

    def click_for(self, x, y, name, want, timeout=20.0, tries=3):
        """Clicks, and makes sure the click did something.

        Holding the button for longer makes a lost click rare rather than
        impossible, and rare is what the old harnesses were: they failed
        about one run in ten, on a different check each time, and every one
        of those read as the window manager being broken. So the click is
        checked, and repeated if the screen never changed.

        Repeating is only safe because the condition is looked at first.
        Most of these buttons are toggles, and clicking one twice puts it
        back; what makes that not happen is that a click that worked is seen
        to have worked and is not repeated."""
        last = None
        for attempt in range(tries):
            self.click(x, y)
            w, h, px, shot, ok = self.wait_screen(
                name, want, timeout=timeout / tries)
            last = (w, h, px, shot, ok)
            if ok:
                return last
        # Nothing happened, three times over. Where the pointer was is the
        # thing worth knowing next, and it is on the screen to be read: a
        # click that missed and a click that landed on a button that does
        # not work look identical in the result and not at all alike here.
        print("      clicked %d,%d %d times with no effect; the pointer was "
              "at %s" % (x, y, tries, self.pointer()))
        return last

    def pointer(self, tries=8):
        """Where the pointer is, read off the screen. None when nothing is
        drawing one, which is the case before the desktop starts.

        Read more than once, because the screen lags the mouse. The pointer
        is drawn by the window manager when it next composites a frame, so a
        reading taken straight after a movement is the position before it:
        measured, a walk to the bottom of the screen read as 51 pixels short
        and was right a second later. Acting on the first reading is worse
        than not checking at all, which is what it did: correcting from a
        stale position moved the pointer somewhere it had never been asked
        to go. Two readings that agree is where it came to rest."""
        last = None
        for _ in range(tries):
            at = self._pointer_once()
            if at is not None and at == last:
                return at
            last = at
            time.sleep(0.3)
        return last

    def _pointer_once(self):
        # Named after this process, because the gate runs several of these
        # machines at once and they all write into the same build directory.
        # One deleting the file another was still reading is a check that
        # fails with a permission error from Windows and nothing to do with
        # what it was checking.
        tag = "pointer-%d" % os.getpid()
        ppm = os.path.join(BUILD, tag + ".ppm")
        try:
            w, h, px, _ = self.screen(tag)
        except (RuntimeError, ValueError):
            return None
        finally:
            if os.path.exists(ppm):
                try:
                    os.remove(ppm)
                except OSError:
                    pass
        want = bytes(CURSOR_FILL)
        start = 0
        while True:
            i = px.find(want, start)
            if i < 0:
                return None
            start = i + 1
            if i % 3:
                continue
            y, x = divmod(i // 3, w)
            mx, my = x - CURSOR_FILL_AT[0], y - CURSOR_FILL_AT[1]
            box = (max(mx, 0), max(my, 0), min(mx + 12, w), min(my + 19, h))
            # Both of its colours, because near an edge the glyph is cut off
            # and counting only the lighter one stops finding it: at the
            # bottom of the screen barely half of it is on screen at all.
            # Something else that happens to be exactly one of these two
            # colours is possible; something that is both, in the shape of a
            # pointer, is not.
            if (count_in(px, w, box, CURSOR_FILL) >= CURSOR_FILL_MIN
                    and count_in(px, w, box, CURSOR_EDGE) >= CURSOR_EDGE_MIN):
                return mx, my

    def drag(self, frm, to, steps=8):
        """Press, travel, release, so the manager sees the pointer move
        rather than teleport.

        The travel is relative, because move_to is not usable here: it walks
        the pointer into the top left corner before stepping out to where it
        was asked for, and doing that with a button held drags the window to
        the corner and back once per step. It worked often enough to look
        fine and failed about one run in three, which is the worst way for
        anything to behave."""
        self.move_to(*frm)
        self.send("mouse_button 1", settle=0.3)
        at_x, at_y = frm
        for i in range(1, steps + 1):
            x = frm[0] + (to[0] - frm[0]) * i // steps
            y = frm[1] + (to[1] - frm[1]) * i // steps
            self.send("mouse_move %d %d" % (x - at_x, y - at_y), settle=0.08)
            at_x, at_y = x, y
        self.send("mouse_button 0", settle=0.5)

    # --- the screen --------------------------------------------------------
    def screen(self, name):
        """A picture of the screen, complete.

        screendump does not return until the file is written, and this does
        not return until screendump has, so what comes back is a whole
        image. It used to be a sleep, and a picture read a moment too early
        is a short file: the bottom of the screen reads as black, whatever
        was being counted is not there, and the check fails naming a window
        that had in fact been drawn."""
        ppm = os.path.join(BUILD, name + ".ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        self.send("screendump %s" % ppm.replace("\\", "/"))
        if not os.path.exists(ppm):
            raise RuntimeError("no screenshot from the monitor")
        return read_ppm(ppm) + (ppm,)

    def wait_screen(self, name, want, timeout=30.0, interval=0.25):
        """Takes pictures until one of them satisfies want, or time runs out.

        want is handed (w, h, px) and says yes or no. Returns
        (w, h, px, path, ok), so a check that failed still has the picture
        that failed it to show."""
        end = time.time() + timeout
        while True:
            w, h, px, ppm = self.screen(name)
            if want(w, h, px):
                return w, h, px, ppm, True
            if time.time() >= end:
                return w, h, px, ppm, False
            time.sleep(interval)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# --- the machine -----------------------------------------------------------

class Guest:
    """A booted machine, with a monitor and a serial line that is read.

    The serial line being read matters more than it looks. QEMU writes it to
    a pipe, and a pipe nobody empties fills up: the write blocks, QEMU stops,
    and the guest freezes solid partway through a test. Nothing reports
    that; it looks exactly like a kernel that hung."""

    def __init__(self, disk, size_mb=32, memory=64, machine=None,
                 kernel="zelr.bin", args=None, extra=None,
                 keep=False, reuse=False):
        self.disk = disk
        # Two separate things, and conflating them makes the first machine
        # decline to create the disk at all: keep leaves it behind at the end,
        # reuse starts from what is already there. Testing that something
        # survives being switched off wants the first and then the second.
        self.keep_disk = keep
        if not reuse:
            if os.path.exists(disk):
                os.remove(disk)
            with open(disk, "wb") as f:
                f.truncate(size_mb * 1024 * 1024)

        self.port = free_port()
        cmd = [qemu_path()]
        if machine:
            cmd += ["-machine", machine]
        cmd += ["-kernel", os.path.join(BUILD, kernel), "-m", str(memory),
                "-no-reboot", "-display", "none", "-serial", "stdio",
                "-drive", "file=%s,format=raw,if=ide,index=0" % disk,
                "-monitor", "tcp:127.0.0.1:%d,server,nowait" % self.port]
        # A machine with a screen opens the desktop by itself, and every
        # harness here starts by waiting for a shell prompt on the serial
        # line. So unless a caller asked for something else, this says
        # console, which is the kernel command line word for staying at one.
        # Escape would also do it and is what a person would press; a word
        # on the command line has no timing in it at all.
        if args is None:   cmd += ["-append", "console"]
        elif args:         cmd += ["-append", args]
        # args="" is the real thing: no command line at all, which is
        # what a machine booted from a disc or a stick has.
        if extra:
            cmd += extra

        self.proc = subprocess.Popen(
            cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)

        self._chunks = []
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()
        self._mon = None

    def _drain(self):
        while True:
            chunk = self.proc.stdout.read(1)
            if not chunk:
                return
            with self._lock:
                self._chunks.append(chunk)

    def serial(self):
        with self._lock:
            return b"".join(self._chunks).decode("utf-8", "replace")

    def alive(self):
        return self.proc.poll() is None

    def wait_serial(self, text, timeout=60.0):
        """Waits for the guest to say something. False if it never does, or
        if it stopped running, which is an answer worth having early rather
        than after the full wait."""
        end = time.time() + timeout
        while True:
            if text in self.serial():
                return True
            if not self.alive():
                return text in self.serial()
            if time.time() >= end:
                return False
            time.sleep(0.1)

    def prompts(self):
        """How many times the shell has asked for a command.

        The prompt carries the working directory, so it is `zelr> ` at the
        root and `zelr:/home> ` anywhere else, and the shell starts in
        /home. Waiting for the bare one waits forever."""
        return len(PROMPT.findall(self.serial()))

    def wait_prompt(self, count=1, timeout=60.0):
        """Waits until the shell has prompted this many times in total."""
        end = time.time() + timeout
        while True:
            if self.prompts() >= count:
                return True
            if not self.alive() or time.time() >= end:
                return self.prompts() >= count
            time.sleep(0.1)

    def wait_boot(self, timeout=120.0):
        """Up to the first shell prompt. This is the sleep that was 4.5
        seconds: enough on an idle host, and not enough when the gate has
        four machines going at once."""
        if not self.wait_prompt(1, timeout):
            raise Timeout("no shell prompt after %gs:\n%s"
                          % (timeout, self.tail()))
        return True

    def run(self, line, timeout=30.0):
        """Types a command and waits for the shell to finish it, which is
        the next prompt rather than a guess at how long it takes."""
        want = self.prompts() + 1
        self.type(line if line.endswith("\n") else line + "\n")
        self.wait_prompt(want, timeout)
        return self.serial()

    def fresh(self, line, timeout=30.0):
        """What one command printed, rather than everything the machine has
        ever said.

        run hands back the whole console. That is fine for checking something
        appeared and useless for checking something is absent, because it will
        be found in the transcript of an earlier command. Two different tests
        have passed that way while checking nothing at all: a reworded size
        line the boot log still contained, and a deleted file an earlier
        listing still showed."""
        mark = len(self.serial())
        self.run(line, timeout)
        return self.serial()[mark:]

    def type(self, text, gap=0.05):
        """One character at a time with a gap.

        QEMU's stdio serial does not apply back pressure: bytes written
        while the guest has not drained the UART are dropped by the host
        before the kernel sees them. Measured: a 26 byte burst reached the
        interrupt handler as 4 bytes, with the kernel's own ring buffer
        reporting no drops at all."""
        for ch in text:
            self.proc.stdin.write(ch.encode())
            self.proc.stdin.flush()
            time.sleep(gap)

    def monitor(self):
        if self._mon is None:
            self._mon = Monitor(self.port)
        return self._mon

    def tail(self, lines=25):
        return "\n".join(self.serial().splitlines()[-lines:])

    def stop(self):
        if self._mon:
            self._mon.close()
            self._mon = None
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        # Left behind when the caller asked to keep it, so a second machine
        # can be started on what this one wrote.
        if not self.keep_disk and os.path.exists(self.disk):
            try:
                os.remove(self.disk)
            except OSError:
                pass


# --- saying how it went ----------------------------------------------------

class Checks:
    """The list of what was checked, and how it is reported. Identical in all
    three harnesses, including the part where a failure names the pictures it
    failed on."""

    def __init__(self, title):
        self.title = title
        self.items = []
        self.shots = []

    def add(self, name, passed, shot=None):
        self.items.append((name, bool(passed), shot))
        if shot and shot not in self.shots:
            self.shots.append(shot)
        return bool(passed)

    def report(self, keep=False, note=None):
        print("=== %s ===" % self.title)
        for name, ok, _ in self.items:
            print("  %s  %s" % ("PASS" if ok else "FAIL", name))
        print()
        failed = [n for n, ok, _ in self.items if not ok]
        if note:
            print(note)
        if failed:
            print("%s: %d failed" % (self.title, len(failed)))
            bad = [s for n, ok, s in self.items if not ok and s]
            if bad:
                print("screenshots: %s" % ", ".join(sorted(set(bad))))
            keep = True
        else:
            print("%s: all %d checks passed" % (self.title, len(self.items)))
        if not keep:
            for s in self.shots:
                if os.path.exists(s):
                    try:
                        os.remove(s)
                    except OSError:
                        pass
        return 1 if failed else 0


def build_once():
    """The gate builds before it starts anything, and several harnesses then
    run at the same time. A second build rewrites the kernel image another
    machine is reading, which on Windows is a permission error arriving as
    whichever harness lost the race reporting its own feature broken."""
    if os.environ.get("ZELR_PREBUILT") == "1":
        return
    subprocess.run(["bash", "build.sh"], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
