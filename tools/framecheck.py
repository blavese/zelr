"""What a frame actually costs.

The desktop draws the whole screen into a back buffer whenever anything
changes, and it used to send all of it to the card every time as well.
Moving the pointer changes something, so moving the pointer cost a three
megabyte write across the bus and, under VMware, an instruction to the host
to look at all of it again. That is a frame rate fixed by the size of the
screen rather than by how much of it is different, which is what "a bit
laggy" was, and it is why giving the machine more of anything did not help.

It sends the bands of rows that differ now, and hands half of the comparison
to a second processor when there is one. Both of those are easy claims to
make and easy to be wrong about, so this measures them: it drives the
pointer around the desktop and then asks the kernel what the frames it drew
actually sent.

  python tools/framecheck.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (Guest, Checks, build_once, count_in, kernel_symbol,
                     CURSOR_FILL, MOUSE_GAP, ROOT)            # noqa: E402

DISK = os.path.join(ROOT, "framecheck.%d.img" % os.getpid())

PAGE = (0x10, 0x14, 0x1A)          # the terminal's default background
TERM_CURSOR = (0x5E, 0xD1, 0xA0)   # and its cursor (term.c, "slate")

# The rounded square behind the first desktop icon, which is lighter while
# the pointer is over the icon's cell (draw_desk_icon: the cell starts at
# ICON_LEFT 18, ICON_TOP 20 and is 92 wide; the tile is 32 in the middle of
# it, with 10 of square round it). ICON_POINT is in the cell and right of
# the square, so the arrow drawn there does not land on what is measured.
ICON_SQUARE = (38, 10, 90, 62)
ICON_POINT = (100, 40)

# The dock's "zelr" badge, against the left of the dock (deskcheck's DOCK_BADGE:
# 76 wide from x 32, 30 tall from y 717), measured left of where the pointer
# is put on it.
BADGE_SEEN = (34, 719, 94, 745)
BADGE_POINT = (98, 722)

# The terminal's close button (deskcheck's BTN_CLOSE: 30 by 24 from x 853, y
# 41), and the corner of it below and right of where the pointer is put, which
# is the part a title bar redrawn too short would leave unlit.
CLOSE_POINT = (858, 42)
CLOSE_LOW = (872, 54, 882, 64)


def reddish(px, w, rect):
    """Pixels in a rectangle that are the close button's red over a title bar."""
    left, top, right, bottom = rect
    n = 0
    for y in range(top, bottom):
        for x in range(left, right):
            i = (y * w + x) * 3
            if px[i] > 170 and px[i + 1] < 120 and px[i + 2] < 120:
                n += 1
    return n


def stats(vm):
    """The numbers /sys/screen reports, as a dictionary."""
    out = vm.run("cat /sys/screen", timeout=20)
    got = {}
    for line in out.splitlines():
        bits = line.split()
        if len(bits) == 2 and bits[1].lstrip("-").isdigit():
            got[bits[0]] = int(bits[1])
    return got


def leave(vm):
    """Out of the desktop and back at the shell. Waited for as one prompt
    more than there were: there has been at least one since boot, so
    waiting for "a prompt" returns at once, and what is typed next lands in
    a desktop that has not finished closing."""
    want = vm.prompts() + 1
    vm.type("\x1b")
    vm.wait_prompt(want, timeout=30)


def arrow_pixels(px, w, x, y):
    """How much of the pointer's fill is in the twelve by nineteen patch it
    is drawn in, with its tip at (x, y)."""
    return count_in(px, w, (x, y, x + 12, y + 19), CURSOR_FILL)


def light(px, w, rect):
    """All the light in a rectangle, added up."""
    left, top, right, bottom = rect
    n = 0
    for y in range(top, bottom):
        row = y * w * 3
        n += sum(px[row + left * 3:row + right * 3])
    return n


def main():
    build_once()
    c = Checks("what a frame costs")

    # Two processors, because half the comparison is meant to go to the
    # second one and a machine with one would say nothing about that.
    vm = Guest(DISK, memory=512, extra=["-smp", "2"])
    try:
        vm.wait_boot()
        mon = vm.monitor()

        before = stats(vm)
        full = before.get("fullkib", 0)
        c.add("the screen says how big a whole frame is", full > 1000)

        vm.type("desktop\n")
        mon.wait_screen("fr-desktop",
                        lambda w, h, px: count_in(px, w, (0, 0, w, h), PAGE) > 250000,
                        timeout=60)

        # Whole frames: a click asks for the whole screen, whatever it
        # changes, and on bare desktop -- right of the terminal, which opens
        # past the icons and is 762 wide -- it changes next to nothing. So
        # each is the whole desktop drawn and compared, and a band or two
        # sent, which is the claim about sending.
        for i in range(12):
            mon.click(950, 150 + (i % 3) * 60)

        # The pointer, moved along the dock. The dock draws a highlight under
        # the pointer, and every one of these moves was a whole frame. The
        # dock draws its hover inside itself, so each now draws only the dock
        # -- and on the way there the icons and the terminal's title bar,
        # which the walk to the corner crosses, only themselves. Counted by
        # the kernel over the moves alone.
        drawn_at = kernel_symbol("draws", "wm.c")
        part_at = kernel_symbol("partial_draws", "wm.c")
        hover_at = kernel_symbol("hover_frames", "wm.c")
        dock_d0, dock_p0, dock_h0 = (mon.read_u32(drawn_at), mon.read_u32(part_at),
                                     mon.read_u32(hover_at))
        for x in range(300, 900, 60):
            mon.move_to(x, 735)
        dock_draws = mon.read_u32(drawn_at) - dock_d0
        dock_partial = mon.read_u32(part_at) - dock_p0
        dock_hover = mon.read_u32(hover_at) - dock_h0

        leave(vm)
        after = stats(vm)

        drew = after["frames"] - before["frames"]
        sent = after["sentkib"] - before["sentkib"]
        shared = after["shared"] - before["shared"]

        # What drawing them cost, apart from sending them: a measurement for
        # whoever changes the drawing next, and a check only that it is
        # counted at all.
        draws = after.get("draws", 0) - before.get("draws", 0)
        spent = after.get("drawmc", 0) - before.get("drawmc", 0)
        c.add("the screen says what drawing the frames cost", draws > 10 and spent > 0)
        if draws > 0:
            print("      %d frames drawn at %d thousand cycles each"
                  % (draws, spent * 1000 // draws))

        c.add("the desktop drew frames", drew > 10)
        if drew > 0:
            each = sent // drew
            print("      %d frames, %d KiB sent, %d KiB a frame against a "
                  "whole screen of %d KiB" % (drew, sent, each, full))
            print("      %d of them were shared with the other processor"
                  % shared)
            c.add("a frame sends less than the whole screen", each < full)
            c.add("and well under half of it", each * 2 < full)
            c.add("the other processor took half of some of them", shared > 0)

        print("      %d frames drawn over the moves along the dock, %d of them only"
              " what they were over (%d as a hover)" % (dock_draws, dock_partial, dock_hover))
        c.add("a move along the dock or across a title bar draws only what it is over",
              dock_draws > 10 and dock_partial == dock_draws and dock_hover > 10)

        # --- and over a window's contents ----------------------------------
        #
        # There nothing on the desktop draws anything for the pointer, and a
        # move drew the whole desktop all the same, wallpaper to dock, to put
        # an arrow twelve pixels wide somewhere else. Such a move puts back
        # what the arrow covered and draws it again now, and /sys/screen
        # counts it. And the arrow has to be where it was sent and gone from
        # where it was: that is what a patch put back wrong would show.
        #
        # The trail is looked for over bare desktop with every window put
        # away (alt+d), and walked there in small steps, because anything
        # that draws a whole frame tidies away an arrow left behind before a
        # picture can see it: a terminal's blinking cursor, move_to itself,
        # which walks through the icons in the corner on the way to anywhere,
        # and the dock's clock, which drew one every second and now draws
        # one a minute.
        before = stats(vm)
        vm.type("desktop\n")
        mon.wait_screen("fr-desktop2",
                        lambda w, h, px: count_in(px, w, (0, 0, w, h), PAGE) > 250000,
                        timeout=60)

        # A title bar's button under the pointer, which is now drawn as the
        # title bar alone: all of the button has to light, down to its lower
        # corner, not only the part a shorter strip would cover.
        mon.move_to(*CLOSE_POINT)
        _, _, _, close_shot, close_lit = mon.wait_screen(
            "fr-close", lambda w, h, px: reddish(px, w, CLOSE_LOW) >= 60, timeout=5)
        path = ((300, 300), (360, 320), (420, 340), (480, 360), (540, 380),
                (600, 400), (540, 380), (480, 360), (420, 340), (360, 320))
        for x, y in path:                  # over the terminal's contents
            mon.move_to(x, y)
        mon.send("sendkey alt-d", settle=1.5)
        mon.move_to(300, 300)
        mon.wait_screen("fr-bare", lambda w, h, px: arrow_pixels(px, w, 300, 300) > 20,
                        timeout=15)
        # And the kernel's own count of frames drawn, read through the
        # monitor, says whether one came along in the middle: a picture that
        # a whole frame has tidied proves nothing, so the walk is tried again.
        steps = 6
        been = [(300 + 20 * i, 300 + 10 * i) for i in range(steps)]
        for _ in range(3):
            mon.move_to(300, 300)
            mon.wait_screen("fr-bare",
                            lambda w, h, px: arrow_pixels(px, w, 300, 300) > 20,
                            timeout=15)
            first = mon.read_u32(drawn_at)
            for _ in range(steps):         # and over bare desktop, 20 across a step
                mon.send("mouse_move 20 10", settle=MOUSE_GAP)
            # Waited for, since the desktop takes the last move when it next
            # looks, which is after the move has been sent.
            w, h, px, shot, _ = mon.wait_screen(
                "fr-inside",
                lambda w, h, px: arrow_pixels(px, w, 420, 360) > 20
                                 and all(arrow_pixels(px, w, x, y) == 0 for x, y in been),
                timeout=5)
            untidied = mon.read_u32(drawn_at) == first
            if untidied:
                break
        now = arrow_pixels(px, w, 420, 360)
        left = sum(arrow_pixels(px, w, x, y) for x, y in been)

        # And the icons, which light up under the pointer. A move onto one
        # has to draw the whole frame, or the icon stays dark with the
        # pointer sitting on it.
        dark = light(px, w, ICON_SQUARE)
        mon.move_to(*ICON_POINT)
        _, _, _, icon_shot, lit = mon.wait_screen(
            "fr-icon", lambda w, h, px: light(px, w, ICON_SQUARE) > dark + 5000,
            timeout=5)

        leave(vm)
        after = stats(vm)
        only = after.get("pointeronly", 0) - before.get("pointeronly", 0)
        drawn = after.get("draws", 0) - before.get("draws", 0)
        print("      %d frames drawn in the whole of that session" % drawn)
        c.add("a move over a window's contents or bare desktop redraws only the pointer",
              only >= 30)
        print("      %d pointer moves redrew only the pointer (the %d sent over the"
              " terminal arrive as several steps each)" % (only, len(path)))
        c.add("and the pointer is where it was sent and nowhere it has been",
              now > 20 and left == 0 and untidied, shot)
        if not (now > 20 and left == 0 and untidied):
            print("      %d pixels of arrow where it is, %d where it has been, %s"
                  % (now, left, "no frame in between" if untidied
                     else "and a whole frame was drawn in between, three times"))
        c.add("an icon still lights up under the pointer", lit, icon_shot)
        c.add("and a title bar's button lights all the way down", close_lit, close_shot)

        # --- and with every window put away --------------------------------
        #
        # A window that is not on the screen has nothing to draw, but one put
        # away asked for a whole frame whenever it redrew all the same, and a
        # terminal redraws twice a second to blink its cursor. And the dock
        # drew one every second for a clock that moves once a minute. So a
        # desktop with nothing on it drew three frames of nothing a second.
        #
        # Counted with the kernel's own count, read through the monitor,
        # over a stretch in the middle of the session: /sys/screen can only
        # be read once the desktop has been left, and opening one draws a
        # dozen frames of its own. The desktop opens with a new terminal,
        # which is waited for, counted while it blinks -- which says the
        # count is being read at all -- and put away.
        vm.type("desktop\n")
        mon.wait_screen("fr-desktop3",
                        lambda w, h, px: count_in(px, w, (0, 0, w, h), PAGE) > 250000,
                        timeout=60)
        # These are measurements of how much happens in a length of time, so
        # they are the one place a length of time is what is waited for.
        #
        # A blinking cursor is the frame an idle desktop draws most, and it
        # drew the whole desktop, wallpaper to dock, while the terminal
        # copied its whole surface out to change one bar. Now the terminal
        # hands over its bottom row (win_commit_rect), the kernel copies out
        # of that only the pixels that differ from what is shown, and the
        # frame draws only those: the kernel counts frames that drew only
        # what changed, the bytes commits looked at and the bytes they
        # copied.
        commits_at = kernel_symbol("published_frames", "winsrv.c")
        copied_at = kernel_symbol("published_bytes", "winsrv.c")
        compared_at = kernel_symbol("compared_bytes", "winsrv.c")
        first = mon.read_u32(drawn_at)
        part0 = mon.read_u32(part_at)
        commits0 = mon.read_u32(commits_at)
        copied0 = mon.read_u32(copied_at)
        compared0 = mon.read_u32(compared_at)
        time.sleep(4)
        showing = mon.read_u32(drawn_at) - first
        partial = mon.read_u32(part_at) - part0
        commits = mon.read_u32(commits_at) - commits0
        copied = mon.read_u32(copied_at) - copied0
        compared = mon.read_u32(compared_at) - compared0

        # A character typed at it, which is a whole commit: every program
        # but the terminal's blink commits the whole of its surface, and
        # what that copies is now what changed -- the letter and the cursor
        # moving on. Waited for as the commit arriving.
        typed_c0 = mon.read_u32(commits_at)
        typed_b0 = mon.read_u32(copied_at)
        mon.send("sendkey x")
        end = time.time() + 10
        while mon.read_u32(commits_at) == typed_c0 and time.time() < end:
            time.sleep(0.1)
        typed_commits = mon.read_u32(commits_at) - typed_c0
        typed_copied = mon.read_u32(copied_at) - typed_b0
        mon.send("sendkey backspace")

        # And it still blinks where it can be seen: the pixels of its colour
        # go up by the bar's worth and back again. The colour is also the
        # prompt's, which is why it is a difference rather than a count.
        counts = []

        def blinking(w, h, px):
            counts.append(count_in(px, w, (0, 0, w, h), TERM_CURSOR))
            return len(counts) > 1 and max(counts) - min(counts) >= 20
        _, _, _, blink_shot, blinked = mon.wait_screen("fr-blink", blinking,
                                                       timeout=10, interval=0.1)

        # --- the dock over a maximised window -------------------------------
        #
        # A maximised window has the whole screen, and the dock comes back
        # over it when the pointer reaches the bottom. A move along the dock
        # then counted as a move over that window's contents, which draws
        # nothing for the pointer, so the dock's hover was never drawn: the
        # pointer sat on the badge and the badge stayed as it was.
        #
        # The bottom row is the window's border, not its contents, so the
        # pointer is brought up into the dock first: the move that matters is
        # from one point of the window's contents under the dock to another.
        mon.send("sendkey alt-f", settle=1.0)
        mon.move_to(200, 767)
        mon.wait_screen(
            "fr-dock-up", lambda w, h, px: count_in(px, w, BADGE_SEEN, PAGE) < 50,
            timeout=15)
        mon.send("mouse_move 0 -27", settle=MOUSE_GAP)
        w, h, px, _, _ = mon.wait_screen(
            "fr-dock-in", lambda w, h, px: arrow_pixels(px, w, 200, 740) > 20, timeout=15)
        dark = light(px, w, BADGE_SEEN)
        mon.send("mouse_move %d %d" % (BADGE_POINT[0] - 200, BADGE_POINT[1] - 740),
                 settle=MOUSE_GAP)
        _, _, _, badge_shot, badge_lit = mon.wait_screen(
            "fr-badge", lambda w, h, px: abs(light(px, w, BADGE_SEEN) - dark) > 3000,
            timeout=5)
        mon.send("sendkey alt-f", settle=1.0)
        mon.send("sendkey alt-d")
        mon.wait_screen("fr-away",
                        lambda w, h, px: count_in(px, w, (0, 0, w, h), PAGE) < 1000,
                        timeout=15)
        # And once the frames that putting it away takes have stopped.
        last = mon.read_u32(drawn_at)
        for _ in range(20):
            time.sleep(0.5)
            seen = mon.read_u32(drawn_at)
            if seen == last:
                break
            last = seen
        time.sleep(8)
        idle = mon.read_u32(drawn_at) - last
        leave(vm)
        print("      %d frames in four seconds with a terminal showing, %d in eight with"
              " every window put away" % (showing, idle))
        # Measured: eight or nine on an idle host, five or six on a busy
        # one. Anything at all says the count is being read.
        c.add("the desktop's count of frames can be read while it is up", showing >= 2)
        print("      %d of those drew only what changed; %d commits looked at %d KiB"
              " and copied %d bytes" % (partial, commits, compared // 1024, copied))
        c.add("a blinking cursor draws only what changed", showing >= 2 and partial == showing)
        # A bar two pixels wide is a hundred and some bytes; the whole
        # surface is 1428 KiB.
        c.add("a blink copies only the pixels that changed",
              commits >= 2 and copied // commits < 8 * 1024)
        # Measured: 118 KiB for the bottom row and what is under it.
        c.add("and the terminal hands over its bottom row, not all of itself",
              commits >= 2 and compared // commits < 256 * 1024)
        print("      a typed character: %d commits copying %d bytes" % (typed_commits, typed_copied))
        c.add("a character typed, a whole commit, copies only what it changed",
              typed_commits >= 1 and typed_copied // typed_commits < 32 * 1024)
        c.add("and the cursor still blinks on the screen", blinked, blink_shot)
        c.add("the dock lights under the pointer over a maximised window", badge_lit, badge_shot)
        # One allowed, for the clock's minute.
        c.add("a desktop with every window put away draws nothing", idle <= 1)

        # --- under a wallpaper that moves -----------------------------------
        #
        # One that moves is painted afresh twelve times a second, and every
        # frame under one was the whole desktop, since a rectangle of it
        # painted at another moment would not meet the rest. It is painted at
        # the moment of the last whole frame now (wall_now), so a blink is a
        # rectangle again; the selftest compares the pixels. The stars.
        vm.run("write /zelr.cfg wallpaper 4")
        vm.type("desktop\n")
        mon.wait_screen("fr-stars",
                        lambda w, h, px: count_in(px, w, (0, 0, w, h), PAGE) > 250000,
                        timeout=60)
        part_at = kernel_symbol("partial_draws", "wm.c")
        first, part0 = mon.read_u32(drawn_at), mon.read_u32(part_at)
        time.sleep(4)
        starry = mon.read_u32(drawn_at) - first
        starry_part = mon.read_u32(part_at) - part0
        leave(vm)
        print("      under the stars: %d frames in four seconds, %d of them only what changed"
              % (starry, starry_part))
        c.add("under a wallpaper that moves, a blink still draws only what changed",
              starry >= 20 and starry_part >= 2)
    finally:
        vm.stop()

    return c.report()


if __name__ == "__main__":
    sys.exit(main())
