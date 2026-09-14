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
#                    boot paths, and the three harnesses that drive the
#                    desktop and the keyboard                            ~18 min
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

failures=0
report() {
  if [ "$2" -eq 0 ]; then printf '  PASS  %s\n' "$1"
  else printf '  FAIL  %s\n' "$1"; failures=$((failures + 1)); fi
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
  ( "$@" > "$f" 2>&1; echo "rc=$?" >> "$f" ) &
  par_names+=("$name")
  par_files+=("$f")
  par_pids+=("$!")
}

par_wait() {                       # collect everything par_start launched
  local i
  for i in "${!par_pids[@]}"; do wait "${par_pids[$i]}" 2>/dev/null; done
  for i in "${!par_names[@]}"; do
    local f="${par_files[$i]}"
    local rc
    rc="$(grep -m1 '^rc=' "$f" | cut -d= -f2)"
    grep -v '^rc=' "$f" | tail -3 | sed 's/^/        /'
    report "${par_names[$i]}" "${rc:-1}"
  done
  rm -rf "$PARALLEL_DIR"
  PARALLEL_DIR=""
  par_names=(); par_files=(); par_pids=()
}

echo "=== gate ($MODE) ==="

# --- it has to build at all ------------------------------------------------
#
# Nothing else runs if this fails. build/nyx.bin is whatever the last
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

# Warnings are not failures, but a build that started producing them is
# something a person should see rather than have buried.
warncount="$(printf '%s' "$build_out" | grep -c 'warning:')"
[ "$warncount" -gt 0 ] && printf '        (%s build warnings)\n' "$warncount"

# --- the kernel's own checks ----------------------------------------------
#
# A fresh disk each time: a test that passes only because a previous run left
# a file behind is worse than no test.
selftest() {
  rm -f gate.img
  head -c 33554432 /dev/zero > gate.img
  local out
  out="$(timeout 300 "$QEMU" -kernel build/nyx.bin -m 256 -no-reboot \
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
  out="$(timeout 300 "$QEMU" -machine q35 -kernel build/nyx.bin -m 256 -no-reboot \
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
shelltest() { timeout 400 bash tools/shell_test.sh 2>&1 | grep -q "all checks passed"; }
par_start "the shell answers over serial" shelltest

# --- the black box, which needs two boots to check at all -----------------
#
# In fast rather than full because everything about running on real hardware
# depends on it: if this is broken, the first failure on a laptop is a black
# screen with nothing behind it, and every other check here is being run
# against a machine that can no longer explain itself.
bbtest() { timeout 400 bash tools/blackbox_test.sh 2>&1 | grep -q "all checks passed"; }
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
  gpttest() { timeout 600 bash tools/gpt_test.sh 2>&1 | grep -q "all checks passed"; }
  par_start "gpt is read, and refused when it does not add up" gpttest

  fat32test() { timeout 600 bash tools/fat32_test.sh 2>&1 | grep -q "all checks passed"; }
  par_start "fat32 is read and written, and survives a reboot" fat32test

  nvmetest() { timeout 600 bash tools/nvme_test.sh 2>&1 | grep -q "all checks passed"; }
  par_start "nvme is a disk, partitioned and not" nvmetest

  # And the kernel's own checks once more, on the third driver. The block
  # layer splits a request differently for each one, and that splitting is
  # what was silently broken on AHCI for as long as it existed.
  selftest_nvme() {
    rm -f gatenv.img
    head -c 67108864 /dev/zero > gatenv.img
    local out
    out="$(timeout 300 "$QEMU" -machine q35 -kernel build/nyx.bin -m 256 -no-reboot \
        -display none -serial stdio -append selftest \
        -drive "file=gatenv.img,format=raw,if=none,id=nv0" \
        -device nvme,drive=nv0,serial=nyx0001 \
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
  boottest() { timeout 900 bash tools/iso_test.sh 2>&1 | grep -q "all four paths passed"; }
  par_start "all four boot paths" boottest

  # --- the parts only a screenshot can check ------------------------------
  shottest() { timeout 600 python tools/shotcheck.py 2>&1 | grep -q "all checks passed"; }
  par_start "the desktop reaches the screen" shottest

  termtest() { timeout 900 python tools/termcheck.py 2>&1 | grep -q "all 8 checks passed"; }
  par_start "typing reaches the terminal" termtest

  desktest() { timeout 900 python tools/deskcheck.py 2>&1 | grep -q "checks passed"; }
  par_start "the windows go where they are told" desktest

  par_wait
fi

# --- tidy up after ourselves ----------------------------------------------
rm -f gate.img gateq.img gatenv.img deskcheck.img termcheck.img shotcheck.img sel.img blackbox.img 2>/dev/null
rm -f gpttest.*.img fat32test.*.img nvmetest.*.img 2>/dev/null
rm -f fat32probe.*.txt fat32high.*.txt 2>/dev/null
rm -f build/*.ppm 2>/dev/null

echo
if [ "$failures" -eq 0 ]; then
  echo "gate ($MODE): everything passed"
  exit 0
fi
echo "gate ($MODE): $failures failed"
exit 1
