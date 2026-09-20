#!/usr/bin/env bash
# Whether the partition reader reads a real table, and refuses a broken one.
#
# The kernel's own checks run against a disk image, which is one filesystem
# written across the whole of a disk and therefore the one case the partition
# code exists because it is not. So the tables are built here by
# tools/mkgpt.py and the kernel is booted against them.
#
# Both halves matter, and the second more than the first. Reading a good GPT
# is a feature; refusing a bad one is the difference between a kernel that
# ignores a corrupt table and a kernel that reads a partition from whatever
# the corruption happened to leave in the entry array.
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
IMG="gpttest.$$.img"
OUT=$(mktemp)
trap 'rm -f "$IMG" "$OUT"' EXIT

type_line() {
  local s="$1" i
  for (( i=0; i<${#s}; i++ )); do printf '%s' "${s:$i:1}"; sleep 0.05; done
  printf '\n'
  sleep 0.45
}

boot() {
  # The reboot at the end is what stops the machine. Without it nothing ever
  # told it to stop, so every boot ran until `timeout` killed it — a fixed
  # 90 seconds whatever the work took — and these were the slowest checks
  # in the gate by a wide margin.
  #
  # A reboot rather than a poweroff, and that is the whole point: poweroff
  # flushes to disk on the way out, and what is being checked here is what
  # reached the disk without anybody flushing it. A reboot is exactly as
  # abrupt as the kill it replaces. qemu exits rather than restarting
  # because -no-reboot is already in the line below.
  { sleep 6; type_line "cat /HELLO.TXT"; sleep 2; type_line "reboot"; sleep 2; } \
    | timeout 90 "$QEMU" -kernel build/zelr.bin -m 256 -no-reboot -display none \
        -append console \
        -serial stdio -drive "file=$IMG,format=raw,if=ide,index=0" \
        > "$OUT" 2>&1 || true
}

fails=0
check()     { if grep -qF "$1" "$OUT"; then echo "  PASS  $2"; else echo "  FAIL  $2  (wanted: $1)"; fails=$((fails+1)); fi; }
check_not() { if grep -qF "$1" "$OUT"; then echo "  FAIL  $2  (found: $1)"; fails=$((fails+1)); else echo "  PASS  $2"; fi; }

echo "=== partition tables ==="

# --- a table that is correct ----------------------------------------------
echo "--- a good gpt ---"
python tools/mkgpt.py "$IMG" > /dev/null
boot
check     "parts gpt, 2 partition(s)"   "the table is read as gpt"
check     "efi system partition"        "the esp is recognised"
check_not "mounted partition 1"         "the esp is not mounted"
check     "fs mounted partition 2"      "the data partition is mounted"
check     "read from a gpt partition"   "a file in it can be read"

# --- a header whose checksum does not match -------------------------------
#
# The protective MBR is still there and still says one partition of type 0xEE
# covering the disk. A kernel that fell back to it would hand out the whole
# disk as a volume, which is exactly the damage the protective entry exists
# to prevent, so the fallback has to skip it and end with no partitions.
echo "--- a corrupt gpt header ---"
python tools/mkgpt.py "$IMG" 65536 --bad-header-crc > /dev/null
boot
check     "gpt rejected: header checksum" "a bad header checksum is caught"
check_not "fs mounted partition"          "nothing is mounted from it"
check_not "read from a gpt partition"     "no file is served from it"

# --- an entry array that does not match its checksum ----------------------
echo "--- a corrupt gpt entry array ---"
python tools/mkgpt.py "$IMG" 65536 --bad-entry-crc > /dev/null
boot
check     "gpt rejected: entry array checksum" "a bad entry checksum is caught"
check_not "fs mounted partition"               "nothing is mounted from it"

# --- no table at all, which is every other test in this project -----------
echo "--- an unpartitioned image ---"
rm -f "$IMG"
head -c 33554432 /dev/zero > "$IMG"
boot
check     "parts none"          "a bare image is treated as one volume"
check     "new disk prepared"   "and is still formatted as before"

echo
if [ "$fails" -eq 0 ]; then echo "all checks passed"; exit 0; fi
echo "$fails failed"
exit 1
