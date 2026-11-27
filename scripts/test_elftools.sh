#!/usr/bin/env bash
# Phase 17 inc.6: cat and wc as ring-3 ELFs.
# (i) From the shell, exec cat.elf on a known file
#     reproduces it byte-for-byte on the console.
# (ii)-(iv) tooltest.elf wires cat.elf | wc.elf through
#     SYS_EXEC_INHERIT: exact wc counts for empty,
#     no-trailing-newline and 2000-byte payloads; a
#     byte-exact 2000-byte cat round trip through pipes;
#     EOF observed after the children's closes; and cat
#     on a nonexistent file exits with code 2, collected
#     via SYS_WAIT.
set -u
cd "$(dirname "$0")/.."
IMG=build/disk.img
[ -f "$IMG" ] || { echo "[test] FAIL: $IMG missing"; exit 1; }

WORK=$(mktemp -d)
cp "$IMG" "$WORK/disk.img"

SER="$WORK/serial.log"
MON="$WORK/mon.sock"
qemu-system-x86_64 -m 64 -display none -serial "file:$SER" \
  -monitor "unix:$MON,server,nowait" \
  -drive file="$WORK/disk.img",format=raw,if=ide,index=0 \
  -boot order=d,menu=off -cdrom build/myos.iso &
QPID=$!
cleanup() { kill $QPID 2>/dev/null; wait $QPID 2>/dev/null; }
trap cleanup EXIT

send() { echo "$1" | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1; }
count() { grep -o -F "$1" "$2" 2>/dev/null | wc -l; }

type_word() {
  local w="$1" i c k
  for (( i=0; i<${#w}; i++ )); do
    c="${w:i:1}"
    case "$c" in
      ' ') k="spc" ;;
      '.') k="dot" ;;
      '-') k="minus" ;;
      '/') k="slash" ;;
      *)   k="$c" ;;
    esac
    send "sendkey $k"
    sleep 0.06
  done
  send "sendkey ret"
  sleep 0.6
}

wait_for_count() {
  local s="$1" want="$2" n=0
  for _ in $(seq 1 90); do
    n=$(count "$s" "$SER")
    [ "$n" -ge "$want" ] && return 0
    sleep 2
  done
  echo "[test] timed out waiting for '$s' >= $want (saw $n)"
  return 1
}

fail() { echo "[test] FAIL: $1"; tail -30 "$SER" 2>/dev/null; exit 1; }
check() { # check <string> <expected-count>
  local n
  n=$(count "$1" "$SER")
  [ "$n" -eq "$2" ] || fail "count of '$1' is $n, expected $2"
}

for _ in $(seq 1 30); do [ -S "$MON" ] && break; sleep 1; done
[ -S "$MON" ] || fail "monitor socket never appeared"
wait_for_count "MyOS" 1 || fail "kernel banner missing"
sleep 10

type_word "write words.txt alpha beta gamma"
wait_for_count "written" 1 || fail "fixture write failed"
type_word "exec cat.elf words.txt"
wait_for_count "alpha beta gamma" 1 || fail "standalone cat did not print words.txt"

type_word "exec tooltest.elf"
wait_for_count "tooltest: ALL PASS" 1 || fail "tooltest did not pass"

type_word "exec argtest.elf one two"
wait_for_count "argc=3" 1 || fail "argtest did not run after tooltest"

type_word "frobnicate alive"
wait_for_count "echo: frobnicate alive" 1 || fail "shell not dispatching afterwards"
sleep 2

check "tooltest: ok " 5
check "tooltest: FAIL" 0
check "alpha beta gamma" 1
check "argc=3" 1
check "echo: frobnicate alive" 1
check "> " 6

echo "[test] PASS: cat/wc ELFs byte-exact standalone and piped, EOF discipline, error codes"
