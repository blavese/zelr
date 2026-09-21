#!/usr/bin/env bash
# Whether the tree is in a state worth keeping.
#
# This is the only thing in the pipeline that decides anything. Neither agent
# gets a say: they write code, this says whether it works, and nothing lands
# on main without it passing. An agent that reports success is not evidence;
# this is.
#
#   gate.sh fast     build, the kernel's checks on two different machines,
#                    the serial shell test, the boot log across a reboot  ~4 min
#   gate.sh screen   the above, plus everything that drives the desktop, the
#                    terminal, the windows, USB and the input path    ~5 min
#   gate.sh full     the above, plus the disks (gpt, fat32, nvme) and all four
#                    boot paths                                       ~17 min
#
# Steps that do not share state run at once. Each boots its own machine and
# builds its own disk, named after its own process id, so the only thing they
# contend for is the host.
#
# fast runs after every change, because a fast check that runs is worth more
# than a thorough one that gets skipped. full runs before anything reaches
# main, because the boot paths and the window manager are exactly where this
# project's real bugs have been.
#
# screen exists because four of full's steps take six minutes each and all
# four are about disks and boot sectors. A change to the keyboard, the
# compositor or the window manager cannot reach any of them, and paying
# seventeen minutes to be told so is how a gate stops being run.

set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

MODE="${1:-fast}"
case "$MODE" in
  fast|screen|full) ;;
  *) echo "gate.sh [fast|screen|full]"; exit 2 ;;
esac
QEMU="${QEMU:-/c/Program Files/qemu/qemu-system-x86_64.exe}"
[ -x "$QEMU" ] || QEMU="$(command -v qemu-system-x86_64 || echo "$QEMU")"

# --- one at a time ---------------------------------------------------------
#
# Every step here boots a machine, builds a disk in the tree and drives the
# screen of whatever it booted. Two gates at once therefore do not get twice
# as much done, they get half as much done twice while contending for the
# same files. That happened three times in one day, and each time the result
# was a list of failures with nothing to do with the code. Refusing is
# better than remembering not to.
LOCK="${TMPDIR:-/tmp}/zelr-gate.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
  echo "a gate is already running here (pid $(cat "$LOCK/pid" 2>/dev/null))."
  echo "wait for it, or remove $LOCK if it is not."
  exit 2
fi
echo $$ > "$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT INT TERM

# And nothing else should be using the machine either. One virtual machine
# left open in a window is enough to make the screen harnesses miss their
# timing, which is how three of them were blamed on the kernel.
others=$(ps -W 2>/dev/null | grep -ciE 'qemu-system|vmware-vmx|VirtualBox' || true)
if [ "${others:-0}" -gt 0 ]; then
  echo "note: $others virtual machine(s) are already running; close them, or"
  echo "      the screen checks will be sharing this host with them"
fi

failures=0
started_at=$(date +%s)
report() {
  local mark="PASS"
  if [ "$2" -ne 0 ]; then mark="FAIL"; failures=$((failures + 1)); fi
  if [ -n "${3:-}" ]; then printf '  %s  %-56s %4ds\n' "$mark" "$1" "$3"
  else                     printf '  %s  %s\n' "$mark" "$1"; fi
}

run_step() {
  local name="$1"; shift
  local out
  out="$("$@" 2>&1)"
  local rc=$?
  printf '%s\n' "$out" | tail -3 | sed 's/^/        /'
  report "$name" "$rc"
  return $rc
}

# --- running several steps at once ----------------------------------------
#
# Every step here is a QEMU boot, and QEMU boots are the whole cost: the full
# run was fifty five minutes, almost all of it one machine at a time while
# the host sat idle. Nothing in these steps shares state. They each build
# their own disk, and since the harnesses started naming those after their
# own process id, two of them can run at once without one deleting the
# other's image.
#
# Output has to be held rather than printed, or three machines interleave
# their lines and none of it can be read. Each step writes to its own file
# and they are reported in a fixed order when all have finished, so the
# result reads exactly as it did when this was serial.
PARALLEL_DIR=""
par_names=()
par_files=()
par_pids=()
par_cmds=()

# How many machines may run at once.
#
# It used to be all of them: the last group starts thirteen, and thirteen
# emulated machines on one host is not thirteen times the work done, it is
# one host that cannot keep up. The checks that suffer are the ones that
# measure something in real time — a sound coming back at the pitch it was
# sent, a pointer arriving where it was put, a keystroke reaching a window —
# and they fail for reasons that have nothing to do with what they check.
#
# Four is not much slower. A group takes as long as its slowest member
# whatever the limit is, and the slowest member here is four minutes.
PAR_MAX="${PAR_MAX:-4}"

par_start() {                      # par_start <name> <function>
  local name="$1"; shift
  [ -n "$PARALLEL_DIR" ] || PARALLEL_DIR="$(mktemp -d)"
  while [ "$(jobs -rp | wc -l)" -ge "$PAR_MAX" ]; do sleep 1; done
  local f="$PARALLEL_DIR/step$(( ${#par_names[@]} )).txt"
  ( s=$(date +%s); "$@" > "$f" 2>&1; rc=$?
    echo "rc=$rc" >> "$f"; echo "secs=$(( $(date +%s) - s ))" >> "$f" ) &
  par_names+=("$name")
  par_files+=("$f")
  par_pids+=("$!")
  par_cmds+=("$*")                 # kept so a failure can be tried once more
}

par_wait() {                       # collect everything par_start launched
  local i
  for i in "${!par_pids[@]}"; do wait "${par_pids[$i]}" 2>/dev/null; done

  local again_names=() again_cmds=()
  for i in "${!par_names[@]}"; do
    local f="${par_files[$i]}"
    local rc secs
    rc="$(grep -m1 '^rc=' "$f" | cut -d= -f2)"
    secs="$(grep -m1 '^secs=' "$f" | cut -d= -f2)"
    # Three lines is the right amount for a step that passed and nowhere
    # near enough for one that did not.
    if [ "${rc:-1}" -eq 0 ]; then
      grep -vE '^(rc|secs)=' "$f" | tail -3 | sed 's/^/        /'
      report "${par_names[$i]}" 0 "${secs:-0}"
    else
      grep -vE '^(rc|secs)=' "$f" | tail -12 | sed 's/^/        /'
      printf '  ....  %-56s %4ds\n' \
             "${par_names[$i]} (busy host; again, alone)" "${secs:-0}"
      again_names+=("${par_names[$i]}")
      again_cmds+=("${par_cmds[$i]}")
    fi
  done

  # A step that failed gets one more go, by itself.
  #
  # Everything above ran at once, and what they contend for is the host:
  # four emulated machines on one. A check that drives the desktop sends a
  # click as a walk and a drag as nine messages, and a starved guest drops
  # some of them, which reads as a window manager that will not resize, or
  # a dealer that will not deal, or a calculator that cannot divide.
  #
  # Measured, over three runs of this gate on one tree: two failures, then
  # one, then two, never the same check twice, and every one of them
  # passing on its own straight afterwards. PAR_MAX going from thirteen to
  # four made that rarer and did not make it stop.
  #
  # So the second result is the one that counts. This forgives a busy host
  # and nothing else: a real failure fails alone as well, and takes the
  # gate down with it. The first failure is still printed in full and the
  # line above says a second go was needed, because a check that needs two
  # is worth knowing about even when the second one passes.
  if [ "${#again_names[@]}" -gt 0 ]; then
    for i in "${!again_names[@]}"; do
      local s2 rc2
      s2=$(date +%s)
      ${again_cmds[$i]} > "$PARALLEL_DIR/again$i.txt" 2>&1
      rc2=$?
      if [ "$rc2" -eq 0 ]; then
        tail -3 "$PARALLEL_DIR/again$i.txt" | sed 's/^/        /'
      else
        tail -12 "$PARALLEL_DIR/again$i.txt" | sed 's/^/        /'
      fi
      report "${again_names[$i]}, alone" "$rc2" "$(( $(date +%s) - s2 ))"
    done
  fi

  rm -rf "$PARALLEL_DIR"
  PARALLEL_DIR=""
  par_names=(); par_files=(); par_pids=(); par_cmds=()
}

# --- deciding a step, without throwing away the evidence -------------------
#
# Every harness here prints a line when it is happy and a list of what broke
# when it is not. These steps used to be written as a pipe straight into
# grep -q, which decided correctly and discarded the rest: a failing step
# reported FAIL and one blank line, and the only way to find out what had
# actually happened was to run the harness again by hand. That is a slow way
# to learn something the run already knew.
#
# So the output is held. On success nothing is printed. On failure the end of
# it is, with the PASS lines dropped so what is left is the failures.
#
# The decision is the exit status, not a phrase in the output. Matching a
# phrase makes the wording part of the contract, and nothing says so: two of
# these harnesses were reworded from "all checks passed" to "all 9 checks
# passed", which is the same harness reporting the same success, and the gate
# called both of them failures while printing the word PASS underneath. Every
# one of these exits non-zero when it fails, which is the thing actually
# worth asking about.
keep() {
  local out rc
  out="$("$@" 2>&1)"; rc=$?
  if [ "$rc" -eq 0 ]; then return 0; fi
  printf '%s\n' "$out" | grep -vE '^\s*PASS' | tail -10
  return 1
}

echo "=== gate ($MODE) ==="

# --- it has to build at all ------------------------------------------------
#
# Nothing else runs if this fails. build/zelr.bin is whatever the last
# successful build left there, so carrying on would test the previous version
# and say something true about code that no longer exists.
build_out="$(bash build.sh 2>&1)"
if printf '%s' "$build_out" | grep -qE '\berror\b'; then
  printf '%s\n' "$build_out" | grep -E '\berror\b' | head -5 | sed 's/^/        /'
  report "it builds" 1
  echo
  echo "gate ($MODE): it does not build, so nothing else was run"
  exit 1
fi
report "it builds" 0

# Every harness below builds for itself, which is right when one is run
# on its own and wrong here. These run several at a time, and four
# builds writing one build directory means one process opens a file
# another is still writing: on Windows that is a permission error, and
# it arrives as whichever harness lost the race reporting that the
# feature it tests is broken. The tree has just been built, so say so.
export ZELR_PREBUILT=1

# Warnings are not failures, but a build that started producing them is
# something a person should see rather than have buried.
warncount="$(printf '%s' "$build_out" | grep -c 'warning:')"
[ "$warncount" -gt 0 ] && printf '        (%s build warnings)\n' "$warncount"

# What the kernel says its version is, which is also what goes inside the
# downloads. It is edited by hand and nothing depended on it, so it has
# drifted twice: once three releases behind, once one.
# Decided on the exit code, not on a word in the output: "version" appears
# in what it prints when it is happy and in what it prints when it is not,
# so matching that would have passed either way.
vercheck() { python tools/check_version.py; }
run_step "the version is not behind the newest tag" vercheck

# Every struct that crosses a system call is declared twice, once for the
# kernel and once for programs, and nothing makes the two agree. The kernel
# copies the size of its own, so a field that is wider on its side is written
# past the end of the program's variable. That has happened twice, and cost a
# day each time. This reads both headers, and takes no time at all.
abicheck() { python tools/abicheck.py; }
run_step "the kernel and its programs agree on the structs" abicheck

# --- and on what a machine with no settings file looks like ---------------
#
# The other half of the same problem. The kernel holds the defaults because
# it draws a desktop before anybody has written a file; the settings program
# holds them because it shows controls before it has read one. When the two
# disagree, the desktop comes up one way, Settings reports another, and
# changing anything at all in that window rewrites the file out of Settings'
# idea of the world. Reads both files; takes no time at all.
defaultcheck() { python tools/defaultcheck.py; }
run_step "and on what a new machine looks like" defaultcheck

# --- the kernel's own checks ----------------------------------------------
#
# A fresh disk each time: a test that passes only because a previous run left
# a file behind is worse than no test.
selftest() {
  rm -f gate.img
  head -c 33554432 /dev/zero > gate.img
  local out
  out="$(timeout 300 "$QEMU" -kernel build/zelr.bin -m 256 -no-reboot \
      -display none -serial stdio -append selftest \
      -drive "file=gate.img,format=raw,if=ide,index=0" \
      -device isa-debug-exit,iobase=0xf4,iosize=0x04 2>&1)"
  rm -f gate.img
  printf '%s\n' "$out" | grep -E 'FAIL|passed,' | tail -3
  printf '%s' "$out" | grep -q SELFTEST_PASS
}
par_start "the kernel's own checks" selftest

# --- and the checks the kernel cannot run ---------------------------------
#
# Floating point, the heap, and JavaScript are all ring 3. The kernel's own
# self test reaches none of them: it runs inside a kernel compiled without
# the vector instructions, with no allocator and no interpreter, before
# there is a program at all. These are programs, and this runs them.
ring3test() { keep timeout 600 python tools/ring3check.py; }
par_start "what a program can do that it could not" ring3test

# --- and again on a machine made this century ------------------------------
#
# The default QEMU machine is a 1996 chipset: no PCIe, so configuration space
# goes through the port pair, and the disk is the ATA driver. q35 has an MCFG
# table and an AHCI controller, so it exercises the mapped path and the other
# driver. Every check is the same; what differs is the hardware underneath.
#
# This is here because both bugs found on the day it was added were invisible
# to the run above: the block layer would not split a request past eight
# sectors, which only AHCI refuses, and ACPI was never given the pointer the
# UEFI loader had already found. Both would have shown up first on a laptop.
selftest_q35() {
  rm -f gateq.img
  head -c 33554432 /dev/zero > gateq.img
  local out
  out="$(timeout 300 "$QEMU" -machine q35 -kernel build/zelr.bin -m 256 -no-reboot \
      -display none -serial stdio -append selftest \
      -drive "file=gateq.img,format=raw,if=none,id=d0" \
      -device ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0 \
      -device isa-debug-exit,iobase=0xf4,iosize=0x04 2>&1)"
  rm -f gateq.img
  printf '%s\n' "$out" | grep -E 'FAIL|passed,' | tail -3
  printf '%s' "$out" | grep -q SELFTEST_PASS
}
par_start "the same checks on q35, with pcie and ahci" selftest_q35

# --- the shell, over the serial line --------------------------------------
shelltest() { keep timeout 400 bash tools/shell_test.sh; }
par_start "the shell answers over serial" shelltest

# --- the black box, which needs two boots to check at all -----------------
#
# In fast rather than full because everything about running on real hardware
# depends on it: if this is broken, the first failure on a laptop is a black
# screen with nothing behind it, and every other check here is being run
# against a machine that can no longer explain itself.
bbtest() { keep timeout 400 bash tools/blackbox_test.sh; }
par_start "the boot log survives a reboot" bbtest

par_wait


if [ "$MODE" = "full" ]; then
  # --- the disks and the things written on them ---------------------------
  #
  # These run here rather than in fast because each one boots the machine
  # several times, and fast has to stay quick enough that it is actually
  # used. They are the first thing full does, because storage is where this
  # project's last several real bugs have been and there is no point
  # spending half an hour on the desktop harnesses if the disk is wrong.
  #
  # A disk image is one filesystem written across the whole of a disk, and
  # every other test in this project uses one. None of what these cover is
  # reachable that way: a partition table, the variant of FAT that every EFI
  # System Partition uses, or a controller that is not AHCI or ATA.
  gpttest() { keep timeout 600 bash tools/gpt_test.sh; }
  par_start "gpt is read, and refused when it does not add up" gpttest

  fat32test() { keep timeout 600 bash tools/fat32_test.sh; }
  par_start "fat32 is read and written, and survives a reboot" fat32test

  nvmetest() { keep timeout 600 bash tools/nvme_test.sh; }
  par_start "nvme is a disk, partitioned and not" nvmetest

  # Copy and paste, which needs a real key press on real hardware to check
  # at all: the control bit has to survive the keyboard driver, the window
  # manager and a system call, and each of those has dropped it.
  cliptest() { keep timeout 400 python tools/clipcheck.py; }
  par_start "copy and paste moves text out of a program" cliptest

  # And the kernel's own checks once more, on the third driver. The block
  # layer splits a request differently for each one, and that splitting is
  # what was silently broken on AHCI for as long as it existed.
  selftest_nvme() {
    rm -f gatenv.img
    head -c 67108864 /dev/zero > gatenv.img
    local out
    out="$(timeout 300 "$QEMU" -machine q35 -kernel build/zelr.bin -m 256 -no-reboot \
        -display none -serial stdio -append selftest \
        -drive "file=gatenv.img,format=raw,if=none,id=nv0" \
        -device nvme,drive=nv0,serial=zelr0001 \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 2>&1)"
    rm -f gatenv.img
    printf '%s\n' "$out" | grep -E 'FAIL|passed,' | tail -3
    printf '%s' "$out" | grep -q SELFTEST_PASS
  }
  par_start "the same checks again, on an nvme disk" selftest_nvme

  par_wait
fi

if [ "$MODE" = "full" ]; then
  # --- every way the machine can be started -------------------------------
  #
  # BIOS and UEFI, disc and stick. This is where the bugs that only appear on
  # a stricter machine than QEMU have all been.
  boottest() { keep timeout 900 bash tools/iso_test.sh; }
  par_start "all four boot paths" boottest
fi

if [ "$MODE" = "screen" ] || [ "$MODE" = "full" ]; then
  # --- the parts only a screenshot can check ------------------------------
  shottest() { keep timeout 600 python tools/shotcheck.py; }
  par_start "the desktop reaches the screen" shottest

  termtest() { keep timeout 900 python tools/termcheck.py; }
  par_start "typing reaches the terminal" termtest

  desktest() { keep timeout 900 python tools/deskcheck.py; }

  # The only check in this project that says anything about whether zelr
  # would take a keystroke on a laptop. Everything else types over the serial
  # line or at a PS/2 keyboard, and a machine built this decade has neither.
  usbtest() { keep timeout 600 python tools/usbcheck.py; }

  # Machines that were interfered with while they were starting, which is
  # what people actually do and what nothing else here covered.
  inputtest() { keep timeout 600 python tools/inputcheck.py; }

  # Notes played, and then measured in a recording of what came out, which is
  # the only part of an audio driver that cannot report success while silent.
  soundtest() { keep timeout 600 python tools/soundcheck.py; }

  # The same notes out of the other controller. VMware gives a guest whose
  # kind it does not recognise an Ensoniq rather than HD Audio, and it does
  # not recognise this one, so the card most people will actually meet is
  # the one the check above does not touch.
  enstest() { keep timeout 600 python tools/enscheck.py; }

  # And the slider on the dock, which is the one place sound is a feature
  # rather than a driver: the note it plays is played at the level it was
  # just set to, so the check is that the quiet one is quieter.
  voltest() { keep timeout 600 python tools/volcheck.py; }

  # And what a frame costs. The desktop sends the bands of the screen that
  # differ from the last one rather than all of it, and hands half of the
  # comparison to a second processor; both are easy to claim and easy to
  # break silently, because a screen that is drawn correctly the slow way
  # looks exactly like one drawn correctly the fast way.
  frametest() { keep timeout 600 python tools/framecheck.py; }

  # A stick with a filesystem on it, mounted and copied to and from.
  mounttest() { keep timeout 600 python tools/mountcheck.py; }

  # Files with the names they were given, across a restart, read back by
  # something that is not this kernel.
  nametest() { keep timeout 600 python tools/namecheck.py; }

  # And the machine turning itself off.
  powertest() { keep timeout 600 python tools/powercheck.py; }

  # The programs that come with it: a calculator checked by making it work
  # out a sum and then typing the answer in by hand, and a music player
  # checked by recording what came out of the machine.
  apptest() { keep timeout 600 python tools/appcheck.py; }
  shtest() { keep timeout 600 python tools/shcheck.py; }

  # Every number the desktop is drawn from is a line in a file now. This
  # asks the kernel to describe its own settings, writes one from the
  # console and watches the dock move, and then works the same control in
  # the settings window and watches the same thing happen. The last of
  # those is the one that matters: the window used to keep its own copy of
  # the list and delete from the file every key it had not heard of.
  settest() { keep timeout 600 python tools/setcheck.py; }

  # The picture decoders, the rasteriser and the layout, which are
  # arithmetic and need no screen at all. They are checked against answers
  # produced by somebody else's libraries on the host, and for a while
  # nothing ran them: ninety odd checks sitting in the image where only
  # somebody who knew their names would find them.
  pictest() { keep timeout 600 python tools/piccheck.py; }

  # The network, from the icon on the panel to an address. The reply comes
  # from QEMU's own DHCP server rather than from anything here, which is the
  # only reason getting one proves anything. Run twice, with a card and
  # without, because a panel that says the same either way reads nothing.
  nettest() { keep timeout 900 python tools/netcheck.py; }

  # And pages off a web server that is not part of this project, so what the
  # client gets right is measured against somebody else's implementation
  # rather than against another half of the same idea.
  webtest() { keep timeout 900 python tools/webcheck.py; }

  # And the browser on top of it: a page off that server, drawn, with its
  # links clicked and its history walked.
  #
  # One step, and quick now for a reason that has nothing to do with this
  # file: it used to sleep a fixed nine to fourteen seconds at every page it
  # visited. It was briefly three steps as well, which made it flaky and
  # starved whatever ran beside it -- every poll in there writes a two
  # megabyte screenshot, and three at once is a gigabyte of writes.
  browsertest() { keep timeout 600 python tools/browsercheck.py; }

  # A page that does its work after it has been read: on a click, on a
  # timer, out of a file of its own, and from an answer it asked the
  # network for. Three of those four were working and unchecked, which
  # is how the readme came to say none of them existed.
  livetest() { keep timeout 700 python tools/livecheck.py; }

  # Filling a form in and sending it, checked against what the server was
  # actually sent rather than against what the browser says it sent. The
  # second is the client marking its own work and passes just as happily
  # with the escaping wrong.
  formtest() { keep timeout 600 python tools/formcheck.py; }

  # What travels, rather than what is shown: a compressed body put back
  # together, a connection kept between requests, and a cookie that comes
  # back. The middle one is counted by the server, because a client that
  # opened one connection per request looks the same from inside itself.
  wiretest() { keep timeout 600 python tools/wirecheck.py; }

  # The second boot of a machine, which is the one nothing else reaches:
  # every other check starts the kernel directly and never asks a firmware
  # to start from the disk the kernel formatted.
  boottest() { keep timeout 900 python tools/bootcheck.py; }

  # ctrl+f, and that what it finds is a word on the screen rather than a
  # program in a list. The desktop cannot read a window, so this is really
  # a check that the programs say what they are showing.
  findtest() { keep timeout 600 python tools/findcheck.py; }

  # The two card games on the screen. What the rules are worth is checked
  # inside the machine by cardtest, which ring3test runs; this is the half
  # that needs a window -- a Games row that opens onto something, cards
  # that reach the cloth, and a dealer that plays its own hand out.
  gametest() { keep timeout 900 python tools/gamecheck.py; }

  # That what the desktop composites is a frame a program had finished. A
  # window's pixels used to be the pixels the program was still writing, so
  # there was no moment at which a frame became finished.
  teartest() { keep timeout 600 python tools/tearcheck.py; }

  # A program the kernel was not built with, run by typing its name. The
  # loading was never the missing half: both shells looked in /bin and
  # nowhere else, and /bin is what is pasted into the kernel image.
  progtest() { keep timeout 600 python tools/progcheck.py; }

  # The power going out in the middle of a write. kernel/fat.c claims
  # there is no moment where neither the old file nor the new one is
  # live; this kills the machine to find out. Six boots, so it is not
  # cheap and it is the only check of a claim that otherwise fails once,
  # on somebody's real disk, months later.
  crashtest() { keep timeout 900 python tools/crashcheck.py; }

  # A program built outside this repository, from sdk/ alone, on a volume
  # this repository wrote rather than one the kernel formatted. The one
  # check that says whether zelr can run software it did not write.
  sdktest() { keep timeout 600 python tools/sdkcheck.py; }

  # Programs on more than one processor. Invisible on a machine with one,
  # so this is the one check that asks for four.
  smptest() { keep timeout 600 python tools/smpcheck.py; }

  # https against the actual web. Everything else about TLS is checked
  # against fixed answers, which proves the arithmetic and cannot prove that
  # a real server will talk to it. This needs a working connection and fails
  # without one, on purpose: a machine that cannot reach the web should not
  # be reporting that its https works.
  tlstest() { keep timeout 900 python tools/tlscheck.py; }
  par_start "the windows go where they are told" desktest
  par_start "a usb keyboard and mouse are found and used" usbtest
  par_start "input survives being touched during boot" inputtest
  par_start "notes come out at the pitch they were asked for" soundtest
  par_start "and out of the ensoniq as well" enstest
  par_start "the volume slider can be heard changing the volume" voltest
  par_start "a frame sends the part of the screen that changed" frametest
  par_start "a usb stick mounts, and files copy off it" mounttest
  par_start "files keep the names they were given" nametest
  par_start "the machine turns itself off" powertest
  par_start "an address is asked for and arrives" nettest
  par_start "pages come back off a real web server" webtest
  par_start "https works against the real web" tlstest
  par_start "the browser shows a page and follows a link" browsertest
  par_start "a page does its work on a click, a timer and an answer" livetest
  par_start "a form is filled in and arrives as it was filled in" formtest
  par_start "bodies arrive compressed and connections are kept" wiretest
  par_start "a machine still starts once it has formatted its disk" boottest
  par_start "ctrl+f finds a word that is on the screen" findtest
  par_start "a deal reaches the cloth and the dealer plays it out" gametest
  par_start "what is on the screen is a frame that was finished" teartest
  par_start "a program on the disk runs by typing its name" progtest
  par_start "a file survives the power going out mid-write" crashtest
  par_start "a program built outside the tree runs on the machine" sdktest
  par_start "programs run on more than one processor" smptest
  par_start "the programs it ships with do what they say" apptest
  par_start "a shell, with pipes and redirection" shtest
  par_start "a setting written by hand reaches the screen" settest
  par_start "the decoders and the layout, with no screen at all" pictest

  par_wait
fi

# --- tidy up after ourselves ----------------------------------------------
#
# After ourselves, and not after anybody else. The working files carry the
# process id of the check that made them, and each check removes its own on
# the way out. This sweep is only for the ones left behind by a run that was
# killed, so it has to leave alone anything newer than the moment this run
# started: that is somebody else's, still in use, and deleting it produces a
# builder that cannot find its input, in another terminal, minutes later.
# Which is the failure the process id in those names was added to prevent,
# reintroduced here by a wildcard.
stale() {
  find . -maxdepth 1 -name "$1" ! -newermt "@$started_at" -delete 2>/dev/null
}
rm -f gate.img gateq.img gatenv.img deskcheck.img termcheck.img shotcheck.img sel.img blackbox.img 2>/dev/null
for pat in 'gpttest.*.img' 'fat32test.*.img' 'nvmetest.*.img' 'clipcheck.*.img' \
           'shotcheck.*.img' 'termcheck.*.img' 'deskcheck.*.img' 'usbcheck.*.img' \
           'fat32probe.*.txt' 'fat32high.*.txt'; do
  stale "$pat"
done
rm -f build/*.ppm 2>/dev/null

echo
took=$(( $(date +%s) - started_at ))
if [ "$failures" -eq 0 ]; then
  echo "gate ($MODE): everything passed in $((took / 60))m $((took % 60))s"
  exit 0
fi
echo "gate ($MODE): $failures failed, after $((took / 60))m $((took % 60))s"
exit 1
