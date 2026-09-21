"""The programs that came with this release, driven the way a person would.

Three of them, and the interesting part is that none of these checks reads
any text off the screen.

The calculator is checked by making it work out 78 / 4 and then typing 19.5
into it by hand: if the arithmetic is right the two pictures of the display
are identical, pixel for pixel, and if it is wrong they are not. No character
recognition, no knowledge of the font, and nothing that keeps passing when
the answer changes.

The music player is given a WAV file on a USB stick, and what came out of the
machine is recorded and measured. A player can find a file, parse its header,
open a stream and hand the samples over, and produce silence; the only thing
that cannot be faked is a tone at the pitch that was in the file.

  python tools/appcheck.py
"""
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Guest, Checks, build_once, ROOT      # noqa: E402
from soundcheck import read_wav, loud_runs, frequency_of, close_to  # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
DISK = os.path.join(ROOT, "appcheck.%d.img" % os.getpid())
STICK = os.path.join(ROOT, "appstick.%d.img" % os.getpid())
TONE = os.path.join(ROOT, "apptone.%d.wav" % os.getpid())
REC = os.path.join(ROOT, "apprec.%d.wav" % os.getpid())

TONE_HZ = 440
TONE_MS = 900
TONE_RATE = 22050            # deliberately not the rate the hardware runs at

USB = ["-device", "qemu-xhci,id=xhci",
       "-drive", "if=none,id=stick,format=raw,file=" + STICK.replace("\\", "/"),
       "-device", "usb-storage,bus=xhci.0,drive=stick"]

AUDIO = ["-audiodev", "wav,id=a0,path=" + REC.replace("\\", "/"),
         "-device", "intel-hda",
         "-device", "hda-output,audiodev=a0"]

# The calculator opens as the second window, so it cascades one step in from
# where the terminal is. Its keys are a four by five grid under the display.
#
# Written the way the kernel writes it. Every one of these was a number that
# happened to be right until the frame went from one pixel to four and the
# cascade moved clear of the desktop icons; the clicks then landed a few
# pixels off every key, and the check that the answer was right went on
# passing because both pictures were of a display nothing had reached.
WM_BORDER, WM_TITLE_H = 1, 32
WM_TOP = WM_BORDER + WM_TITLE_H
ICON_LEFT, ICON_CELL_W = 14, 78           # where winsrv.c starts the cascade
CASCADE_X = ICON_LEFT + ICON_CELL_W + 14

CALC_X, CALC_Y = CASCADE_X + 48, 36 + 38  # one step in: the second window
CALC_W, CALC_H = 280, 380                 # what the program asks for

INNER_X, INNER_Y = CALC_X + WM_BORDER, CALC_Y + WM_TOP
UI_PAD, UI_GAP = 8, 6
DISPLAY_H = 86

# Inside the display's well, clear of the bevel round it.
DISPLAY = (INNER_X + UI_PAD + 2, INNER_Y + UI_PAD + 2,
           INNER_X + CALC_W - UI_PAD - 2, INNER_Y + DISPLAY_H - UI_PAD - 2)


def key_at(col, row):
    """The middle of one key.

    The grid is worked out from the window's content size the same way the
    program works it out, because a remembered coordinate is how these
    harnesses break."""
    gx = INNER_X + UI_PAD
    gy = INNER_Y + DISPLAY_H + UI_PAD
    gw = CALC_W - UI_PAD * 2
    gh = CALC_H - DISPLAY_H - UI_PAD * 2
    kw = (gw - UI_GAP * 3) // 4
    kh = (gh - UI_GAP * 4) // 5
    return (gx + col * (kw + UI_GAP) + kw // 2,
            gy + row * (kh + UI_GAP) + kh // 2)


def make_tone():
    """A WAV of one note, written here so the machine has something to play
    that this side knows the pitch of."""
    n = TONE_RATE * TONE_MS // 1000
    body = bytearray()
    for i in range(n):
        # A square wave: no floating point on either side of this, and the
        # frequency is what is being measured rather than the shape.
        half = TONE_RATE // (TONE_HZ * 2)
        v = 9000 if (i // half) % 2 == 0 else -9000
        body += struct.pack("<h", v)

    out = bytearray()
    out += b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    out += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, TONE_RATE,
                                 TONE_RATE * 2, 2, 16)
    out += b"data" + struct.pack("<I", len(body)) + bytes(body)
    with open(TONE, "wb") as f:
        f.write(out)


def make_stick():
    make_tone()
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "mkfat.py"),
                        STICK, "8192", TONE + ":TONE.WAV"],
                       capture_output=True, text=True)
    return r.returncode == 0


def patch(px, w, rect):
    left, top, right, bottom = rect
    out = bytearray()
    for y in range(top, bottom):
        row = y * w * 3
        out += px[row + left * 3:row + right * 3]
    return bytes(out)


def display(mon, name):
    w, h, px, ppm = mon.screen(name)
    try:
        os.remove(ppm)
    except OSError:
        pass
    return patch(px, w, DISPLAY)


def tap(mon, keys):
    """A run of keys, one click each.

    Deliberately not click_for, which is what everything else here reaches
    for when a click has to be seen to have worked. click_for presses again
    when the screen did not change, and its own docstring says what makes
    that safe: the condition is looked at first, so a click that worked is
    never repeated.

    A keypad breaks that rule. "The display changed" cannot tell a click
    that landed from a click that landed twice, and a digit entered twice is
    not a lost keystroke, it is a different sum. Tried, and measured:
    pressing 7 and then 8 through click_for left the display reading 77, and
    the check that followed reported a calculator that could not divide.

    So the keys go in plainly, and it is the whole sequence that is looked
    at and repeated.
    """
    for col, row in keys:
        mon.click(*key_at(col, row))
        time.sleep(0.35)


def main():
    keep = "--keep" in sys.argv
    build_once()
    c = Checks("app test")

    if not make_stick():
        c.add("a stick can be built with a tone on it", False)
        return c.report()

    vm = Guest(DISK, memory=256, machine="q35", extra=USB + AUDIO)
    try:
        vm.wait_boot()
        vm.type("desktop\n")
        time.sleep(6)
        mon = vm.monitor()

        # --- the calculator ------------------------------------------------
        vm.type("calc\n")
        time.sleep(3)

        # What the display says before anything has been pressed, so that
        # two identical pictures of a display nothing reached cannot be read
        # as the arithmetic coming out right. They were, once: the keys had
        # moved and every click was landing between them.
        empty = display(mon, "calc-empty")

        # 78 / 4, and then 19.5 typed in by hand, and the two pictures of
        # the display compared.
        #
        # Up to three goes, from C each time. A click that never arrived is
        # not an answer to "does this divide", and under a loaded host one of
        # the nine does go missing: this failed exactly that way under the
        # gate, having passed on its own a minute before and a minute after.
        # Starting from C is what makes another go safe, because it puts the
        # calculator back to a state this side knows rather than typing
        # further into one it does not.
        worked = typed = None
        for _ in range(3):
            tap(mon, [(0, 0)])                                  # C
            tap(mon, [(0, 1), (1, 1), (3, 0), (0, 2), (3, 4)])  # 7 8 / 4 =
            worked = display(mon, "calc-divided")

            tap(mon, [(0, 0)])                                  # C
            tap(mon, [(0, 3), (2, 1), (2, 4), (1, 2)])          # 1 9 . 5
            typed = display(mon, "calc-typed")
            if worked != empty and worked == typed:
                break

        c.add("the keys reach the calculator at all", worked != empty)
        c.add("the calculator divides, and the answer is the one a person "
              "would have typed", worked == typed)

        # Something that is not the answer, so the check above cannot be
        # passing because both pictures are of an empty display.
        tap(mon, [(1, 3)])                  # 2
        other = display(mon, "calc-other")
        c.add("and a different number looks different", other != worked)

        mon.send("sendkey alt-q", settle=1.0)

        # --- the monitor ----------------------------------------------------
        vm.type("monitor\n")
        time.sleep(3.5)
        mon.move_to(900, 300)
        w, h, px, ppm = mon.screen("mon-before")
        before = patch(px, w, (100, 100, 700, 200))
        os.remove(ppm)

        # Something else to show. count runs for a while and keeps the
        # processor, so both the task list and the graph have to move.
        vm.type("run count &\n")
        w, h, px, shot, moved = mon.wait_screen(
            "mon-after",
            lambda w, h, px: patch(px, w, (100, 100, 700, 200)) != before,
            timeout=25)
        c.add("the monitor notices another program starting", moved, shot)

        mon.send("sendkey alt-q", settle=1.0)

        # --- the music player ------------------------------------------------
        boot = vm.serial()
        c.add("the stick is there to play from", "usb volume mounted" in boot)

        # Started on the file, which is the argument a task can now carry.
        #
        # Twelve seconds rather than six. This is the one check in the
        # gate that measures something in real time — sound coming back
        # out of the machine at the pitch that went in — and six
        # seconds of wall clock on a host running a dozen machines is
        # not six seconds of a guest getting to play anything. It came
        # back silent in the gate and at 440 Hz on its own, twice,
        # which is what that looks like.
        vm.type("music /usb/TONE.WAV\n")
        time.sleep(12)
    finally:
        vm.stop()
        for junk in (DISK, STICK, TONE):
            try:
                os.remove(junk)
            except OSError:
                pass

    # --- and what came out of it --------------------------------------------
    rate, samples = read_wav(REC) if os.path.exists(REC) else (0, [])
    c.add("something was recorded", len(samples) > 0)

    if samples:
        runs = loud_runs(samples, rate)
        c.add("the player made a sound", len(runs) > 0)
        if runs:
            start, end = runs[0]
            heard = frequency_of(samples[start:end], rate)
            c.add("at the pitch that was in the file (%d Hz, heard %d)"
                  % (TONE_HZ, int(heard)), close_to(heard, TONE_HZ, 0.08))

    try:
        os.remove(REC)
    except OSError:
        pass
    return c.report(keep=keep)


if __name__ == "__main__":
    sys.exit(main())
