"""Drags the volume slider and listens to what that does.

The point of the slider making a noise is that how loud it is can be heard
rather than only read, so the thing to check is not that a note was played
but that the note played at a low setting is quieter than the note played at
a high one. Nothing about that can be established from a screenshot.

QEMU writes what the guest played to a file, so this opens the panel on the
dock, drags the slider to one end and then to the other, and measures the two
recordings against each other.

  python tools/volcheck.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Guest, Checks, build_once, count_near, face_width,
                     FACE_HEAD, ROOT)                          # noqa: E402
from soundcheck import read_wav, loud_runs                     # noqa: E402

DISK = os.path.join(ROOT, "volcheck.%d.img" % os.getpid())
WAV = os.path.join(ROOT, "volcheck.%d.wav" % os.getpid())

AUDIO = ["-audiodev", "wav,id=a0,path=" + WAV.replace("\\", "/"),
         "-device", "intel-hda", "-device", "hda-output,audiodev=a0"]

# The dock, worked out the way wm.c works it out. The same arithmetic as
# tools/netcheck.py, and for the same reason: the clock is measured rather
# than assumed, because everything to the left of it moves when the face
# it is set in changes.
SCREEN_W, SCREEN_H = 1024, 768
TASKBAR_H, TASKBAR_GAP, DOCK_SIDE = 44, 14, 16
VOLPOP_W, VOLPOP_H = 208, 40
VOL_W = 30

PANEL_Y = SCREEN_H - TASKBAR_H - TASKBAR_GAP
DOCK_RIGHT = DOCK_SIDE + (SCREEN_W - DOCK_SIDE * 2)
CLOCK_W = face_width("12:54", FACE_HEAD)
VOLUME_X = DOCK_RIGHT - 20 - CLOCK_W - 16 - VOL_W
VOL_ICON = (VOLUME_X + VOL_W // 2, PANEL_Y + TASKBAR_H // 2)

# The panel it opens, and the track inside it.
POP_X = min(max(VOLUME_X + VOL_W // 2 - VOLPOP_W // 2, TASKBAR_GAP),
            SCREEN_W - TASKBAR_GAP - VOLPOP_W)
TRACK_X = POP_X + 16
TRACK_Y = PANEL_Y - VOLPOP_H - 8 + VOLPOP_H // 2 - 3
TRACK_W = VOLPOP_W - 32 - 40
POP_Y = PANEL_Y - VOLPOP_H - 8
POP_RECT = (POP_X, POP_Y, POP_X + VOLPOP_W, POP_Y + VOLPOP_H)

# What the panel is tinted with before the wallpaper behind it shows through.
OVERLAY = (0xF4, 0xF4, 0xF7)


def peak_of(samples):
    top = 0
    for s in samples:
        a = s if s >= 0 else -s
        if a > top:
            top = a
    return top


def main():
    build_once()
    c = Checks("the volume slider")

    for stale in (WAV, DISK):
        if os.path.exists(stale):
            os.remove(stale)

    vm = Guest(DISK, memory=256, extra=AUDIO)
    try:
        vm.wait_boot()
        c.add("the machine has a sound controller", "sound   " in vm.serial())

        mon = vm.monitor()
        vm.type("desktop\n")
        mon.wait_screen("vol-desktop", lambda w, h, px: True, timeout=30)

        # Open the panel on the dock.
        w, h, px, shot, opened = mon.click_for(
            VOL_ICON[0], VOL_ICON[1], "vol-panel",
            lambda w, h, px: count_near(px, w, POP_RECT, OVERLAY, 6) > 3000,
            timeout=20)
        c.add("clicking the speaker opens the volume panel", opened, shot)

        # Quiet first: a fifth of the way along the track. The drag is what a
        # hand does, and it is the drag rather than the click that the notes
        # come from.
        mon.drag((TRACK_X + TRACK_W // 2, TRACK_Y),
                 (TRACK_X + TRACK_W // 5, TRACK_Y))
        time.sleep(1.2)
        # Then loud: all the way to the other end.
        mon.drag((TRACK_X + TRACK_W // 5, TRACK_Y),
                 (TRACK_X + TRACK_W - 2, TRACK_Y))
        time.sleep(1.5)
    finally:
        try:
            vm.monitor().send("quit", settle=0.5)
        except Exception:
            pass
        time.sleep(1.0)
        vm.stop()
        try:
            os.remove(DISK)
        except OSError:
            pass

    if not os.path.exists(WAV):
        c.add("something was recorded", False)
        return c.report()

    rate, samples = read_wav(WAV)
    c.add("something was recorded", len(samples) > 0)

    runs = loud_runs(samples, rate, floor=400, min_ms=30)
    print("      %d burst(s) of sound in %.2fs"
          % (len(runs), len(samples) / float(rate or 1)))
    c.add("dragging the slider makes a noise", len(runs) >= 2)

    if len(runs) >= 2:
        # The level is applied on the way into the buffer, so the note played
        # after the slider was dragged down has to come out smaller than the
        # one played after it was dragged up. That is the whole feature: the
        # noise is not a notification, it is the setting.
        first = peak_of(samples[runs[0][0]:runs[0][1]])
        last = peak_of(samples[runs[-1][0]:runs[-1][1]])
        print("      first burst peaks at %d, last at %d" % (first, last))
        c.add("and the one at the loud end is louder than the one at the "
              "quiet end", last > first * 3 // 2)

    try:
        os.remove(WAV)
    except OSError:
        pass
    return c.report()


if __name__ == "__main__":
    sys.exit(main())
