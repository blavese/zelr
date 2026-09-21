#!/usr/bin/env bash
# Builds the kernel using zig's bundled clang+lld as a cross compiler, so no
# separate x86_64-elf toolchain is needed.
#
# The SSE flags matter and are not optional. SSE2 is part of the x86-64
# baseline, so unlike on 32-bit the compiler assumes it is available and will
# emit xmm registers for ordinary integer and struct copies. A kernel that has
# not set CR4.OSFXSR takes an invalid-opcode fault on the first one, long
# before anything has a way to report it. Turning them off costs nothing here:
# there is no floating point in this kernel.
set -e
cd "$(dirname "$0")"

ZIG="${ZIG:-$(command -v zig || true)}"
if [ -z "$ZIG" ]; then
  ZIG=$(find /c/Users/admin/AppData/Local/Microsoft/WinGet/Packages -iname zig.exe 2>/dev/null | head -1)
fi
[ -z "$ZIG" ] && { echo "zig not found; set ZIG=/path/to/zig" >&2; exit 1; }

# GNU stat and BSD stat spell this differently, and macOS ships the BSD one.
filesize() { stat -c %s "$1" 2>/dev/null || stat -f %z "$1" 2>/dev/null || echo '?'; }

mkdir -p build

# The user programs and the SMP trampoline go inside the kernel image, so
# they are built first.
ZIG="$ZIG" bash userland/build.sh
ZIG="$ZIG" bash bootloader/build.sh
ZIG="$ZIG" bash uefi/build.sh

SRC=$(find boot kernel -name '*.c' -o -name '*.S' | sort | tr '\n' ' ')

# kernel/builtin.S pulls the user programs and the SMP trampoline in through
# .incbin, and zig caches compiled objects without knowing that. Editing a
# user program leaves builtin.S byte for byte identical, so the cache hits and
# the kernel silently ships the copy from the previous build. Folding a
# checksum of the blobs into the command line, which the cache key does cover,
# is what makes a changed program actually reach the image.
BLOBS=$(cat build/user/*.elf build/trampoline.bin | cksum | cut -d' ' -f1)

"$ZIG" cc -target x86_64-freestanding-none \
  -ffreestanding -nostdlib -static -O2 -std=gnu11 \
  -fno-sanitize=undefined -fno-stack-protector -fno-stack-check \
  -fno-builtin -fno-omit-frame-pointer -fno-pic -fno-pie \
  -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-red-zone \
  -mcmodel=small \
  -Wall -Wextra -Wno-unused-parameter \
  -Iinclude -DZELR_BLOB_STAMP=$BLOBS \
  -Wl,-T,linker.ld -Wl,--build-id=none -Wl,-z,max-page-size=4096 \
  -o build/zelr.elf $SRC

# The flat image is what both bootloaders copy into place, and what a
# multiboot loader is given: the a.out kludge in boot/boot.S points at this
# rather than at the ELF, because no multiboot loader will parse a 64-bit one.
#
# Flattened to the address it was linked for, asked rather than repeated. A
# flat image is just bytes and carries no record of where it belongs, so the
# two numbers disagreeing produces a file that looks entirely correct and
# jumps into the middle of itself.
LOAD_AT=$(python tools/loadaddr.py)
python tools/flatten.py build/zelr.elf build/zelr.bin "$LOAD_AT" >/dev/null

echo "built build/zelr.elf ($(filesize build/zelr.elf) bytes)"
echo "      build/zelr.bin ($(filesize build/zelr.bin) bytes)"
