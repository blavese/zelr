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
  # From the root the file is under home: docs was made in /home, where the
  # shell starts. This read docs/inner.txt, which does not exist, and passed
  # for years on the echo of the write two lines up.
  type_line "cat home/docs/inner.txt"
  type_line "ls home/docs"
  type_line "ps"
  type_line "mem"
  type_line "spawn"
  type_line "exec /bin/hello"
  sleep 1.5          # exec prints a lot; let the guest drain first
  type_line "exec /bin/wintest"
  sleep 1.5
  type_line "echo done testing"
  sleep 2.5
  # Stops the machine, so this ends when the typing is done rather
  # than ninety seconds later.
  type_line "reboot"
  sleep 1
}

feed | run_with_timeout 90 "$QEMU" -kernel build/zelr.bin -m 64 -no-reboot -display none -serial stdio -append console > "$OUT" 2>&1 || true

# Without the carriage returns the serial line puts on every line, so that a
# line can be matched whole.
tr -d '\r' < "$OUT" > "$OUT.lf" && mv "$OUT.lf" "$OUT"

fails=0
check() {
  if grep -qF "$1" "$OUT"; then echo "  PASS  $2"
  else echo "  FAIL  $2  (wanted: $1)"; fails=$((fails+1)); fi
}

# The whole of a line, for output that repeats words from the command that
# asked for it. The shell echoes every character typed, so "write notes.txt
# shell wrote this" put "shell wrote this" in the transcript whether or not
# cat ever printed it; four checks here were passing on the echo alone. An
# echoed line starts with the prompt, so it is never a whole-line match.
check_line() {
  if grep -qxF "$1" "$OUT"; then echo "  PASS  $2"
  else echo "  FAIL  $2  (wanted a line: $1)"; fails=$((fails+1)); fi
}

check_re() {
  if grep -qE "$1" "$OUT"; then echo "  PASS  $2"
  else echo "  FAIL  $2  (wanted: $1)"; fails=$((fails+1)); fi
}

echo "=== shell test ==="
check "zelr $VERSION x86_64"        "uname reports the kernel"
check_re '^ +([0-9]+|<dir>)  notes/?$' "ls shows the seeded files"
check "written from scratch"     "cat prints file contents"
check_line "shell wrote this"    "write then cat round trips"
check "no such file"             "cat reports a deleted file as missing"
# pwd's answer on a line of its own; the prompt says /home/docs too.
check_line "/home/docs"          "cd moves into a directory"
check_line "nested file"         "a file written inside one reads back by path"
check "PID"                      "ps prints the task table"
check "running "                 "ps formats task state columns"
check "physical:"                "mem reports physical memory"
check "spawned pid"              "spawn creates a task"
check "hello from a program"     "exec runs a built-in ELF in ring 3"
check "wintest: surface at 0x0000008060000000" "a ring 3 program is handed a window surface"
check "wintest: wrote and read back 3072 pixels" "it can write every pixel of it"
check "closed, handle is dead"  "the handle stops working once closed"
check "wintest: ok"              "and it cannot reach another program's window"
check_line "done testing"        "echo works"
echo
if [ $fails -eq 0 ]; then echo "shell test: all checks passed"; else echo "shell test: $fails failed"; fi
echo "(transcript: $OUT)"
exit $fails
