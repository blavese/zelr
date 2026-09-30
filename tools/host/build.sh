#!/bin/bash
# A ring 3 program built for the Windows host on tools/host/shim.c: see the
# comment at the top of that file for why and what it does.
#
#   bash tools/host/build.sh [program]      # default: browser
#
# The program is userland/<program>.c, or one of the host's own tools in
# tools/host/ (cssq). Writes build/host/<program>.exe, and a .pdb for
# HOST_SAMPLE. Zig is the compiler, as it is for everything else here.
set -e
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
OUT=$ROOT/build/host
PROG=${1:-browser}
mkdir -p "$OUT"

# The SDK's header with its one door into the kernel sent to the shim instead.
python - "$ROOT/sdk/zelr.h" "$OUT/zelr.h" <<'EOF'
import sys
src = open(sys.argv[1], encoding="utf-8").read()
a = src.index("static inline zelr_word syscall(")
b = src.index("return r;\n}", a) + len("return r;\n}")
door = ("zelr_word host_syscall(zelr_word n, zelr_word a, zelr_word b, zelr_word c);\n"
        "static inline zelr_word syscall(zelr_word n, zelr_word a, zelr_word b, zelr_word c) {\n"
        "    return host_syscall(n, a, b, c);\n}")
open(sys.argv[2], "w", encoding="utf-8", newline="\n").write("#define ZELR_HOST 1\n" + src[:a] + door + src[b:])
EOF

SRC=userland/$PROG.c
[ -f "$SRC" ] || SRC=tools/host/$PROG.c
ZIG=${ZIG:-zig}
T="-target x86_64-windows-gnu"
$ZIG cc $T -O2 -g -c tools/host/shim.c -o "$OUT/shim.o"
$ZIG cc $T -O2 -g -ffreestanding -fno-builtin -w -DZELR_NO_START -Dmain=zelr_main \
    -I "$OUT" -I sdk -I userland -c "$SRC" -o "$OUT/$PROG.o"
$ZIG cc $T "$OUT/$PROG.o" "$OUT/shim.o" -o "$OUT/$PROG.exe" -lws2_32 -ldbghelp -lbcrypt \
    -Wl,--stack,33554432
echo "built build/host/$PROG.exe"
