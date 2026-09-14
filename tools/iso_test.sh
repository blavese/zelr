#!/usr/bin/env bash
# Boots the image every way a real machine might, and checks zelr came up.
#
# One file, four paths in. None of them is QEMU's -kernel: every one goes
# through a bootloader of ours, and two of them go through firmware that
# never enters real mode at all.
#
#   BIOS from a disc     El Torito, first catalog entry
#   BIOS from a stick    the MBR at the front of the image
#   UEFI from a disc     El Torito, second catalog entry, platform 0xEF
#   UEFI from a stick    the partition marked as an EFI system partition
set -e
cd "$(dirname "$0")/.."

# Taken from the one place that defines it, so a version bump does not
# quietly turn these checks into ones that can never pass.
VERSION=$(grep KERNEL_VERSION include/types.h | cut -d'"' -f2)

QEMU="${QEMU:-}"
[ -z "$QEMU" ] && QEMU=$(command -v qemu-system-x86_64 || true)
[ -z "$QEMU" ] && QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"

# UEFI needs firmware. QEMU ships it; without it the two UEFI paths are
# skipped rather than reported as failures.
FW="${ZELR_UEFI_FW:-}"
[ -z "$FW" ] && FW="/c/Program Files/qemu/share/edk2-x86_64-code.fd"

python tools/mkiso.py >/dev/null

ISO=build/zelr.iso
DISK=build/isotest.img

fails=0
skipped=0

run() {
  local what="$1"; shift
  local out
  out=$(mktemp)

  rm -f "$DISK"
  head -c 33554432 /dev/zero > "$DISK"

  # Type at it, so the check proves the shell is really running rather than
  # that the banner happened to be printed.
  (sleep 9
   s="write booted.txt $what"
   for (( i=0; i<${#s}; i++ )); do printf '%s' "${s:$i:1}"; sleep 0.05; done
   printf '\n'; sleep 1.5
   s="cat booted.txt"
   for (( i=0; i<${#s}; i++ )); do printf '%s' "${s:$i:1}"; sleep 0.05; done
   printf '\n'; sleep 2.5) \
    | timeout 90 "$QEMU" "$@" -m 256 -no-reboot -display none -serial stdio \
        > "$out" 2>&1 || true

  echo "--- $what ---"
  for want in "zelr $VERSION" "long mode" "progs" "zelr:/home>" "$what"; do
    if grep -qF "$want" "$out"; then echo "  PASS  $want"
    else echo "  FAIL  $want"; fails=$((fails+1)); fi
  done
  rm -f "$out"
}

echo "=== boot test ==="

run "bios from a disc" -cdrom "$ISO" -boot d \
    -drive "file=$DISK,format=raw,if=ide,index=1"

run "bios from a stick" \
    -drive "file=$ISO,format=raw,if=ide,index=0" -boot c \
    -drive "file=$DISK,format=raw,if=ide,index=1"

if [ -f "$FW" ]; then
  run "uefi from a disc" \
      -drive "if=pflash,format=raw,readonly=on,file=$FW" \
      -cdrom "$ISO" -boot d \
      -drive "file=$DISK,format=raw,if=ide,index=1"

  run "uefi from a stick" \
      -drive "if=pflash,format=raw,readonly=on,file=$FW" \
      -drive "file=$ISO,format=raw,if=ide,index=0" \
      -drive "file=$DISK,format=raw,if=ide,index=1"
else
  echo "--- uefi ---"
  echo "  SKIP  no firmware at $FW; set ZELR_UEFI_FW to test the UEFI paths"
  skipped=1
fi

rm -f "$DISK"
echo
if [ $fails -eq 0 ]; then
  if [ $skipped -eq 1 ]; then echo "boot test: the bios paths passed, uefi skipped"
  else echo "boot test: all four paths passed"; fi
else
  echo "boot test: $fails failed"
fi
exit $fails
