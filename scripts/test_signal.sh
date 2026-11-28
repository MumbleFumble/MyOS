#!/usr/bin/env bash
# Phase 18 inc.3: signal vocabulary + wait encoding +
# shell $? surfacing. signaltest.elf proves per-signal
# collection (SIGKILL 4105, SIGTERM 4111, SIGINT 4098),
# invalid signals (-1, target untouched), self-kill by
# signal, and bad-pid. At the prompt: a faulting
# foreground child (exec hostileprobe.elf recurse)
# leaves $? = 1, a clean child (exec argtest.elf)
# leaves $? = 0, and a signal death (exec
# signaltest.elf selfterm) leaves $? = 1.
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
      '>') k="shift-dot" ;;
      '|') k="shift-backslash" ;;
      '=') k="equal" ;;
      '_') k="shift-minus" ;;
      '$') k="shift-4" ;;
      '?') k="shift-slash" ;;
      '{') k="shift-bracket_left" ;;
      '}') k="shift-bracket_right" ;;
      [A-Z]) k="shift-$(echo "$c" | tr 'A-Z' 'a-z')" ;;
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

type_word "exec signaltest.elf"
wait_for_count "signaltest: ALL PASS" 1 || fail "signaltest did not reach ALL PASS"
check "signaltest: ok " 11
check "signaltest: FAIL" 0

type_word "exec hostileprobe.elf recurse"
wait_for_count "> " 3 || fail "prompt did not return after faulting child"
type_word "frobnicate s\$?"
wait_for_count "frobnicate s1" 1 || fail "faulting child did not leave ? = 1"

type_word "exec argtest.elf one two"
wait_for_count "> " 5 || fail "prompt did not return after clean child"
type_word "frobnicate s\$?"
wait_for_count "frobnicate s0" 1 || fail "clean child did not leave ? = 0"

type_word "exec signaltest.elf selfterm"
wait_for_count "> " 7 || fail "prompt did not return after signal-death child"
type_word "frobnicate s\$?"
wait_for_count "frobnicate s1" 2 || fail "signal death did not leave ? = 1"

check "frobnicate s1" 2
check "frobnicate s0" 1
check "> " 8

echo "[test] PASS: per-signal statuses 4098/4105/4111, invalid sig safe, ? surfaces child deaths"
exit 0
