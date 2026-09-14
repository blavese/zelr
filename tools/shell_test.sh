#!/usr/bin/env bash
# Drives the shell over the serial line and checks what comes back.
#
# Input is fed one character at a time with a small gap. QEMU's -serial stdio
# backend does not apply back pressure to a pipe: bytes written while the
# guest has not drained the UART are dropped by the host before the kernel
# ever sees them. (Measured: a 26 byte burst reached the ISR as 4 bytes, with
# the kernel's own ring buffer reporting zero drops.) Typing speed is well
# within what the guest keeps up with.
set -e
cd "$(dirname "$0")/.."

# Taken from the one place that defines it, so a version bump does not
# quietly turn these checks into ones that can never pass.
VERSION=$(grep KERNEL_VERSION include/types.h | cut -d'"' -f2)

QEMU="${QEMU:-}"
[ -z "$QEMU" ] && QEMU=$(command -v qemu-system-x86_64 || true)
[ -z "$QEMU" ] && QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"

run_with_timeout() {
  local seconds="$1"
  shift
  if command -v timeout >/dev/null 2>&1; then
    timeout "$seconds" "$@"
  elif command -v gtimeout >/dev/null 2>&1; then
    gtimeout "$seconds" "$@"
  else
    python3 -c '
import subprocess, sys
p = subprocess.Popen(sys.argv[2:])
try:
    raise SystemExit(p.wait(timeout=float(sys.argv[1])))
except subprocess.TimeoutExpired:
    p.terminate()
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    raise SystemExit(124)
' "$seconds" "$@"
  fi
}

# Built here when this is run on its own. Under the gate it is not: the
# gate has built already, and several harnesses run at once, so a build
# here would rewrite the kernel image another one's QEMU is reading.
[ "${ZELR_PREBUILT:-}" = "1" ] || bash build.sh >/dev/null
OUT=$(mktemp)

type_line() {
  local s="$1"
  local i
  for (( i=0; i<${#s}; i++ )); do
    printf '%s' "${s:$i:1}"
    sleep 0.05
  done
  printf '\n'
  sleep 0.45
}

feed() {
  sleep 2.5
  type_line "uname"
  type_line "ls"
  type_line "cat /doc/readme"
  type_line "write notes.txt shell wrote this"
  type_line "cat notes.txt"
  type_line "rm notes.txt"
  type_line "cat notes.txt"
  type_line "mkdir docs"
  type_line "cd docs"
  type_line "pwd"
  type_line "write inner.txt nested file"
  type_line "cd /"
  type_line "cat docs/inner.txt"
  type_line "ls docs"
  type_line "ps"
  type_line "mem"
  type_line "spawn"
  type_line "exec /bin/hello"
  sleep 1.5          # exec prints a lot; let the guest drain first
  type_line "exec /bin/wintest"
  sleep 1.5
  type_line "echo done testing"
  sleep 2.5
}

feed | run_with_timeout 90 "$QEMU" -kernel build/zelr.bin -m 64 -no-reboot -display none -serial stdio > "$OUT" 2>&1 || true

fails=0
check() {
  if grep -qF "$1" "$OUT"; then echo "  PASS  $2"
  else echo "  FAIL  $2  (wanted: $1)"; fails=$((fails+1)); fi
}

echo "=== shell test ==="
check "zelr $VERSION x86_64"        "uname reports the kernel"
check "notes"                    "ls shows the seeded files"
check "written from scratch"     "cat prints file contents"
check "shell wrote this"         "write then cat round trips"
check "no such file"             "cat reports a deleted file as missing"
check "/home/docs"               "cd moves into a directory"
check "nested file"              "a file written inside one reads back by path"
check "PID"                      "ps prints the task table"
check "running "                 "ps formats task state columns"
check "physical:"                "mem reports physical memory"
check "spawned pid"              "spawn creates a task"
check "hello from a program"     "exec runs a built-in ELF in ring 3"
check "wintest: surface at 0x0000008060000000" "a ring 3 program is handed a window surface"
check "wintest: wrote and read back 3072 pixels" "it can write every pixel of it"
check "closed, handle is dead"  "the handle stops working once closed"
check "wintest: ok"              "and it cannot reach another program's window"
check "done testing"             "echo works"
echo
if [ $fails -eq 0 ]; then echo "shell test: all checks passed"; else echo "shell test: $fails failed"; fi
echo "(transcript: $OUT)"
exit $fails
