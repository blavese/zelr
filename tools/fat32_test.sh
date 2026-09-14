#!/usr/bin/env bash
# Whether FAT32 is read, written, and still right after a reboot.
#
# The kernel's own checks run on a volume this kernel formatted, and it
# formats FAT16. So the variant every EFI System Partition and every memory
# stick larger than two gigabytes actually uses would otherwise never be
# exercised at all.
#
# The reboot is the point. Reading a FAT32 volume proves the layout was
# understood; writing to one and finding it intact on the next boot proves
# the table updates were written with the right width in the right place. A
# cluster number truncated to sixteen bits reads back perfectly within a
# single run, because the same wrong value is used both times.
#
# The working files carry the process id. Two copies of this running at once
# otherwise share names in the repository root, and the first to finish
# deletes the other's probe file out from under it, which looks exactly like
# a builder that cannot find its input.
set -e
cd "$(dirname "$0")/.."

QEMU="${QEMU:-}"
[ -z "$QEMU" ] && QEMU=$(command -v qemu-system-x86_64 || true)
[ -z "$QEMU" ] && QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"

IMG="fat32test.$$.img"
SRC="fat32probe.$$.txt"
HIGH="fat32high.$$.txt"
OUT=$(mktemp)
trap 'rm -f "$IMG" "$SRC" "$HIGH" "$OUT"' EXIT

type_line() {
  local s="$1" i
  for (( i=0; i<${#s}; i++ )); do printf '%s' "${s:$i:1}"; sleep 0.05; done
  printf '\n'
  sleep 0.55
}

boot() {
  { sleep 6; "$1"; sleep 2; } \
    | timeout 120 "$QEMU" -kernel build/zelr.bin -m 256 -no-reboot -display none \
        -serial stdio -drive "file=$IMG,format=raw,if=ide,index=0" \
        > "$OUT" 2>&1 || true
}

fails=0
check()     { if grep -qF "$1" "$OUT"; then echo "  PASS  $2"; else echo "  FAIL  $2  (wanted: $1)"; fails=$((fails+1)); fi; }
check_not() { if grep -qF "$1" "$OUT"; then echo "  FAIL  $2  (found: $1)"; fails=$((fails+1)); else echo "  PASS  $2"; fi; }

echo "=== fat32 ==="

printf 'read from a fat32 volume\n' > "$SRC"
printf 'read from above cluster 65535\n' > "$HIGH"

# HIGH.TXT is put past cluster 65535 on purpose.
#
# A directory entry splits the cluster number across two fields sixteen bytes
# apart, and a reader that takes only the low half is right for every file
# below 65536 and wrong for every file above it. Without a file out there the
# test cannot tell the two apart: an image whose files all sit near the front
# reads identically either way. Measured, before this line existed: with the
# high half deliberately ignored, every check in this file still passed.
python tools/mkfat.py --fat32 "$IMG" 40960 \
    "$SRC:HELLO32.TXT" "$SRC:SUB/INNER.TXT" \
    --at-cluster=70000 "$HIGH:HIGH.TXT" > /dev/null

# --- first boot: read what is there, then write ---------------------------
first() {
  type_line "cd /"
  type_line "cat /HELLO32.TXT"
  type_line "cat /SUB/INNER.TXT"
  type_line "cat /HIGH.TXT"
  type_line "write made32.txt written onto fat32"
  type_line "mkdir newdir"
  type_line "write newdir/deep.txt deep on fat32"
}
boot first

check     "fat32 mounted"                "the volume is recognised as fat32, not fat16"
check_not "fat16 mounted"                "and not mistaken for the other variant"
check     "read from a fat32 volume"     "a file in the root is read"
check     "read from above cluster 65535" "a file past cluster 65535 is read"
check_not "reclaimed"                    "nothing is wrongly reclaimed at mount"

# --- second boot: is any of it still there --------------------------------
second() {
  type_line "cd /"
  type_line "cat /made32.txt"
  type_line "cat /newdir/deep.txt"
  type_line "cat /HELLO32.TXT"
  type_line "cat /HIGH.TXT"
}
boot second

check     "written onto fat32"           "a file written last boot survived"
check     "deep on fat32"                "a file in a directory made last boot survived"
check     "read from a fat32 volume"     "the original file is still readable"
check     "read from above cluster 65535" "and so is the one past cluster 65535"
check_not "reclaimed"                    "the second mount reclaims nothing either"
check_not "no such file"                 "nothing that should be there is missing"

# --- and the other variant still works ------------------------------------
echo "--- fat16, unchanged ---"
rm -f "$IMG"
python tools/mkfat.py "$IMG" 32768 "$SRC:HELLO16.TXT" > /dev/null
sixteen() { type_line "cd /"; type_line "cat /HELLO16.TXT"; }
boot sixteen

check     "fat16 mounted"                "a fat16 volume is still read as fat16"
check_not "fat32 mounted"                "and not mistaken for the other variant"
check     "read from a fat32 volume"     "its contents are readable"

echo
if [ "$fails" -eq 0 ]; then echo "all checks passed"; exit 0; fi
echo "$fails failed"
exit 1
