#!/usr/bin/env bash
# Proves the one claim the black box exists to make: that what the machine
# was doing survives the machine stopping.
#
# The kernel's own checks cover the buffer, the checksum and the refusal to
# write to a disk it did not format. None of that can cover this, because
# this needs two boots: the log has to be written by one run and read by the
# next one, off a disk that outlived the first. So the test boots the same
# image twice and asks the second boot what the first one did.
#
# The care needed here is in where the checks look. Every log line is
# mirrored to serial as it is written, so the second boot prints its own
# marks whether or not anything was ever saved, and a check that searches
# the whole transcript passes with the record-keeping entirely broken.
# (Measured: with the disk write removed, four of seven checks still passed.)
# So the output of `cat` is fenced between two echoes and the checks are
# confined to what lies between them.
set -e
cd "$(dirname "$0")/.."

QEMU="${QEMU:-}"
[ -z "$QEMU" ] && QEMU=$(command -v qemu-system-x86_64 || true)
[ -z "$QEMU" ] && QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"

# The working image carries the process id. Two copies of this running at
# once otherwise share a name in the repository root, and the first to
# finish deletes the other's disk out from under it. That is not
# hypothetical: it happened, and it looked like a builder that could not
# find its input.
IMG="blackbox.$$.img"
OUT1=$(mktemp)
OUT2=$(mktemp)
FENCED=$(mktemp)
trap 'rm -f "$IMG" "$OUT1" "$OUT2" "$FENCED"' EXIT

type_line() {
  local s="$1" i
  for (( i=0; i<${#s}; i++ )); do printf '%s' "${s:$i:1}"; sleep 0.05; done
  printf '\n'
  sleep 0.45
}

boot() {
  timeout 90 "$QEMU" -kernel build/zelr.bin -m 128 -no-reboot -display none \
    -append console \
    -serial stdio -drive "file=$IMG,format=raw,if=ide,index=0" \
    > "$1" 2>&1 || true
}

fails=0
check() {                          # check <text> <file> <what>
  if grep -qF "$1" "$2"; then echo "  PASS  $3"
  else echo "  FAIL  $3  (wanted: $1)"; fails=$((fails+1)); fi
}
check_not() {
  if grep -qF "$1" "$2"; then echo "  FAIL  $3  (found: $1)"; fails=$((fails+1))
  else echo "  PASS  $3"; fi
}

echo "=== black box across a reboot ==="

# A blank disk, so the first boot is the one that formats it and no record
# can have been left behind by an earlier run of this script.
rm -f "$IMG"
head -c 33554432 /dev/zero > "$IMG"

# --- first boot: writes the record, then the power goes off ---------------
{ sleep 6; type_line "echo first boot done"; sleep 1; } | boot "$OUT1"

check "== handing over to the scheduler" "$OUT1" "the first boot ran to the end"
check "first boot done" "$OUT1" "the first boot reached a shell"
check "fs new disk prepared" "$OUT1" "the first boot formatted the disk"

# --- second boot: the disk is the only thing that carried over ------------
{ sleep 6; type_line "echo BEGIN_RECORD"; type_line "cat /sys/lastboot";
  type_line "echo END_RECORD"; sleep 2; } | boot "$OUT2"

sed -n '/BEGIN_RECORD/,/END_RECORD/p' "$OUT2" > "$FENCED"

if [ ! -s "$FENCED" ]; then
  echo "  FAIL  the second boot answered at all"
  echo; echo "1 failed"; exit 1
fi
echo "  PASS  the second boot answered at all"

check_not "no record" "$FENCED" "a record was found"
check "== gdt"      "$FENCED" "the previous boot's early marks are in it"
check "== disk"     "$FENCED" "the previous boot's disk phase is in it"
check "== handing over to the scheduler" "$FENCED" "its last mark is in it"
check "boot log"    "$FENCED" "it identifies itself"

# The decisive one. The first boot found a blank disk and formatted it; this
# boot found a filesystem and mounted it. If what came back says "prepared"
# it is the earlier boot's log and not this one's read back to us.
check "fs new disk prepared" "$FENCED" "it is the earlier boot, not this one"
check_not "fs fat16 mounted" "$FENCED" "this boot's own log did not leak into it"

echo
if [ "$fails" -eq 0 ]; then
  echo "all checks passed"
  exit 0
fi
echo "$fails failed"
exit 1
