#!/usr/bin/env bash
# Whether the NVMe driver is a disk, not just a device that was found.
#
# Finding the controller is the easy half and the half that looks like
# success. What matters is that a sector written through it is the sector
# read back, that a file survives the machine being switched off, and that a
# request larger than one command still works: the driver moves at most a
# page per command, so everything above it depends on the block layer
# splitting correctly against a limit that is different again from the other
# two drivers.
#
# The last case is the one worth the trouble: a GPT-partitioned NVMe disk is
# what a laptop made this decade actually has, and it is the arrangement in
# which nothing else in this project is exercised.
set -e
cd "$(dirname "$0")/.."

QEMU="${QEMU:-}"
[ -z "$QEMU" ] && QEMU=$(command -v qemu-system-x86_64 || true)
[ -z "$QEMU" ] && QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"

IMG="nvmetest.$$.img"
OUT=$(mktemp)
trap 'rm -f "$IMG" "$OUT"' EXIT

type_line() {
  local s="$1" i
  for (( i=0; i<${#s}; i++ )); do printf '%s' "${s:$i:1}"; sleep 0.05; done
  printf '\n'
  sleep 0.55
}

boot() {
  { sleep 6; "$1"; sleep 2; } \
    | timeout 150 "$QEMU" -machine q35 -kernel build/nyx.bin -m 256 -no-reboot \
        -display none -serial stdio \
        -drive "file=$IMG,format=raw,if=none,id=nv0" \
        -device nvme,drive=nv0,serial=nyx0001 \
        > "$OUT" 2>&1 || true
}

fails=0
check()     { if grep -qF "$1" "$OUT"; then echo "  PASS  $2"; else echo "  FAIL  $2  (wanted: $1)"; fails=$((fails+1)); fi; }
check_not() { if grep -qF "$1" "$OUT"; then echo "  FAIL  $2  (found: $1)"; fails=$((fails+1)); else echo "  PASS  $2"; fi; }

echo "=== nvme ==="

rm -f "$IMG"
head -c 67108864 /dev/zero > "$IMG"

# --- first boot: it should find the controller and prepare the disk -------
first() {
  type_line "cd /"
  type_line "write onnvme.txt written through nvme"
  type_line "cat onnvme.txt"
  type_line "mkdir nvdir"
  type_line "write nvdir/deep.txt deep through nvme"
}
boot first

check     "via nvme"                "the controller is found and used"
check_not "disk    none"            "it is not mistaken for no disk at all"
check     "written through nvme"    "a file written through it reads back"

# --- second boot: nothing carried over but the disk itself ---------------
second() {
  type_line "cd /"
  type_line "cat /onnvme.txt"
  type_line "cat /nvdir/deep.txt"
  type_line "cat /sys/lastboot"
}
boot second

check     "written through nvme"    "the file survived the power going off"
check     "deep through nvme"       "so did one in a directory made last boot"
check_not "no such file"            "nothing that should be there is missing"

# The boot log lives in reserved sectors and is written in one request of
# more sectors than this driver takes at once, so finding the previous boot's
# record here is the block layer's splitting working on a third driver.
check     "== handing over to the scheduler" "the previous boot's log was recovered"

# --- a partitioned nvme disk, which is what a laptop has -----------------
echo "--- gpt on nvme ---"
rm -f "$IMG"
python tools/mkgpt.py "$IMG" 65536 > /dev/null
third() { type_line "cd /"; type_line "cat /HELLO.TXT"; }
boot third

check     "via nvme"                     "the partitioned disk is still nvme"
check     "parts gpt"                    "its table is read"
check     "efi system partition"         "the esp is recognised"
check_not "mounted partition 1"          "and left alone"
check     "read from a gpt partition"    "a file in the data partition is read"

echo
if [ "$fails" -eq 0 ]; then echo "all checks passed"; exit 0; fi
echo "$fails failed"
exit 1
