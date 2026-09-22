#!/usr/bin/env bash
# Builds one C program against zelr's libc.
#
#   bash build.sh hello.c            -> hello.elf
#   bash build.sh hello.c mine.elf   -> mine.elf
#
# The difference from sdk/build.sh next door is three lines: the libc's
# headers come before the compiler's idea of where headers live, the libc's
# sources are compiled alongside the program, and the entry point comes from
# crt0.c rather than from the header.
#
# Still one compiler invocation and still no build system. A libc is a dozen
# files rather than one, which is a reason to list them and not a reason to
# introduce make.
set -e
cd "$(dirname "$0")"

SRC="${1:-}"
[ -z "$SRC" ] && { echo "usage: build.sh PROGRAM.c [OUT.elf]" >&2; exit 2; }
case "$SRC" in /*) ;; *) SRC="$OLDPWD/$SRC" ;; esac
[ -f "$SRC" ] || { echo "no such file: $SRC" >&2; exit 2; }

OUT="${2:-${SRC%.c}.elf}"
case "$OUT" in /*) ;; *) OUT="$OLDPWD/$OUT" ;; esac

CC="${CC:-}"
if [ -z "$CC" ]; then
  ZIG="${ZIG:-$(command -v zig || true)}"
  [ -z "$ZIG" ] && { echo "no CC set and no zig on the path" >&2; exit 1; }
  CC="$ZIG cc"
fi

# -nostdinc          so that <stdio.h> is the one in include/ and not the
#                    host's, which would compile and then link against a
#                    libc that is not here
# -DZELR_NO_SUGAR    sdk/zelr.h keeps its system calls and stands aside from
#                    the names C standardised
# -DZELR_NO_START    the entry point comes from crt0.c, once
$CC -target x86_64-freestanding-none \
  -nostdinc -Iinclude -I.. \
  -DZELR_NO_SUGAR -DZELR_NO_START \
  -ffreestanding -nostdlib -static -O2 -std=gnu11 \
  -fno-sanitize=undefined -fno-stack-protector -fno-stack-check \
  -fno-builtin -fno-pic -fno-pie -mcmodel=large \
  -mno-red-zone \
  -Wall -Wextra \
  -Wl,-T,../zelr.ld -Wl,--build-id=none \
  -o "$OUT" \
  "$SRC" \
  src/crt0.c src/string.c src/ctype.c src/stdlib.c src/stdio.c \
  src/math.c src/time.c

echo "built $OUT"
