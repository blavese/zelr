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
#   gate.sh full     the above, plus the disks (gpt, fat32, nvme), all four
#                    boot paths, the three harnesses that drive the desktop
#                    and the keyboard, and a machine with a USB keyboard and
#                    mouse on it                                         ~17 min
#
# Steps that do not share state run at once. Each boots its own machine and
# builds its own disk, named after its own process id, so the only thing they
# contend for is the host.
#
# fast runs after every change, because a fast check that runs is worth more
# than a thorough one that gets skipped. full runs before anything reaches
# main, because the boot paths and the window manager are exactly where this
# project's real bugs have been.

set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

MODE="${1:-fast}"
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

par_start() {                      # par_start <name> <function>
  local name="$1"; shift
  [ -n "$PARALLEL_DIR" ] || PARALLEL_DIR="$(mktemp -d)"
  local f="$PARALLEL_DIR/step$(( ${#par_names[@]} )).txt"
  ( s=$(date +%s); "$@" > "$f" 2>&1; rc=$?
    echo "rc=$rc" >> "$f"; echo "secs=$(( $(date +%s) - s ))" >> "$f" ) &
  par_names+=("$name")
  par_files+=("$f")
  par_pids+=("$!")
}

par_wait() {                       # collect everything par_start launched
  local i
  for i in "${!par_pids[@]}"; do wait "${par_pids[$i]}" 2>/dev/null; done
  for i in "${!par_names[@]}"; do
    local f="${par_files[$i]}"
    local rc secs
    rc="$(grep -m1 '^rc=' "$f" | cut -d= -f2)"
    secs="$(grep -m1 '^secs=' "$f" | cut -d= -f2)"
    # Three lines is the right amount for a step that passed and nowhere
    # near enough for one that did not.
    if [ "${rc:-1}" -eq 0 ]; then
      grep -vE '^(rc|secs)=' "$f" | tail -3 | sed 's/^/        /'
    else
      grep -vE '^(rc|secs)=' "$f" | tail -12 | sed 's/^/        /'
    fi
    report "${par_names[$i]}" "${rc:-1}" "${secs:-0}"
  done
  rm -rf "$PARALLEL_DIR"
  PARALLEL_DIR=""
  par_names=(); par_files=(); par_pids=()
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

  # --- every way the machine can be started -------------------------------
  #
  # BIOS and UEFI, disc and stick. This is where the bugs that only appear on
  # a stricter machine than QEMU have all been.
  boottest() { keep timeout 900 bash tools/iso_test.sh; }
  par_start "all four boot paths" boottest

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
  par_start "the windows go where they are told" desktest
  par_start "a usb keyboard and mouse are found and used" usbtest
  par_start "input survives being touched during boot" inputtest

  par_wait
fi

# --- tidy up after ourselves ----------------------------------------------
rm -f gate.img gateq.img gatenv.img deskcheck.img termcheck.img shotcheck.img sel.img blackbox.img 2>/dev/null
rm -f gpttest.*.img fat32test.*.img nvmetest.*.img clipcheck.*.img 2>/dev/null
rm -f shotcheck.*.img termcheck.*.img deskcheck.*.img usbcheck.*.img 2>/dev/null
rm -f fat32probe.*.txt fat32high.*.txt 2>/dev/null
rm -f build/*.ppm 2>/dev/null

echo
took=$(( $(date +%s) - started_at ))
if [ "$failures" -eq 0 ]; then
  echo "gate ($MODE): everything passed in $((took / 60))m $((took % 60))s"
  exit 0
fi
echo "gate ($MODE): $failures failed, after $((took / 60))m $((took % 60))s"
exit 1
