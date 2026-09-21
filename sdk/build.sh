#!/usr/bin/env bash
# Builds one C file into a zelr program.
#
#   bash build.sh hello.c            -> hello.elf
#   bash build.sh hello.c mine.elf   -> mine.elf
#
# This is one compiler invocation and no build system, because that is all it
# is: a zelr program is a freestanding static executable with no libc, no
# dynamic linking and no relocation. Every flag below is here for a reason
# and README.md says which.
#
# zig is used for its bundled clang and lld, which cross compile to this
# target without a toolchain being installed for it. Any clang and lld will
# do -- set CC to one, and it wants the same flags.
set -e
cd "$(dirname "$0")"

SRC="${1:-}"
[ -z "$SRC" ] && { echo "usage: build.sh PROGRAM.c [OUT.elf]" >&2; exit 2; }
OUT="${2:-${SRC%.c}.elf}"

CC="${CC:-}"
if [ -z "$CC" ]; then
  ZIG="${ZIG:-$(command -v zig || true)}"
  [ -z "$ZIG" ] && { echo "no CC set and no zig on the path" >&2; exit 1; }
  CC="$ZIG cc"
fi

# -mcmodel=large      programs are linked at 0x8040000000, which is further
#                     from zero than a 32-bit displacement reaches
# -mno-red-zone       an interrupt can arrive between any two instructions
#                     and the kernel does not respect the 128 bytes below rsp
# -fno-pic -fno-pie   nothing relocates anything; the address is the address
# -fno-builtin        or the compiler turns a copy loop into a call to memcpy,
#                     which is not here to be called
$CC -target x86_64-freestanding-none \
  -I. \
  -ffreestanding -nostdlib -static -O2 -std=gnu11 \
  -fno-sanitize=undefined -fno-stack-protector -fno-stack-check \
  -fno-builtin -fno-pic -fno-pie -mcmodel=large \
  -mno-red-zone \
  -Wall -Wextra \
  -Wl,-T,zelr.ld -Wl,--build-id=none \
  -o "$OUT" "$SRC"

echo "built $OUT"
