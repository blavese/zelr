#!/usr/bin/env bash
# Builds each user program into a standalone ELF executable. These are not
# linked against the kernel in any way: the only thing they share with it is
# the system call numbers in zelr.h.
set -e
cd "$(dirname "$0")"

ZIG="${ZIG:-$(command -v zig || true)}"
if [ -z "$ZIG" ]; then
  ZIG=$(find /c/Users/admin/AppData/Local/Microsoft/WinGet/Packages -iname zig.exe 2>/dev/null | head -1)
fi
[ -z "$ZIG" ] && { echo "zig not found" >&2; exit 1; }

# GNU stat and BSD stat spell this differently, and macOS ships the BSD one.
filesize() { stat -c %s "$1" 2>/dev/null || stat -f %z "$1" 2>/dev/null || echo '?'; }

mkdir -p ../build/user
for src in *.c; do
  name="${src%.c}"
  "$ZIG" cc -target x86_64-freestanding-none \
    -ffreestanding -nostdlib -static -O2 -std=gnu11 \
    -fno-sanitize=undefined -fno-stack-protector -fno-stack-check \
    -fno-builtin -fno-pic -fno-pie -mcmodel=large \
    -mno-red-zone \
    -Wall -Wextra \
    -Wl,-T,link.ld -Wl,--build-id=none \
    -o "../build/user/$name.elf" "$src"
  echo "  $name.elf  $(filesize "../build/user/$name.elf") bytes"
done
