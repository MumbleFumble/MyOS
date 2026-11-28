#!/usr/bin/env bash
# Phase 18 inc.4: process groups. pgrptest.elf proves
# pgid inheritance (child + grandchild), setpgid moves
# and refusals, one-shot group kill with per-member
# 4096+sig collection, and the group edges. At the
# prompt, pgrptest's pgrp mode observes the shell's
# grouping: an exec'd child reports the shell's pgid
# (inheritance -- a single external command only ever
# runs via exec in this shell), while the last stage
# of a two-stage external pipeline reports a different
# pgid (the first stage's pid), distinct per pipeline.
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

type_word "exec pgrptest.elf"
wait_for_count "pgrptest: ALL PASS" 1 || fail "pgrptest did not reach ALL PASS"
check "pgrptest: ok " 19
check "pgrptest: FAIL" 0

type_word "exec pgrptest.elf pgrp"
wait_for_count "pgrp pid=" 1 || fail "exec pgrp line missing"
type_word "pgrptest.elf pgrp | pgrptest.elf pgrp"
wait_for_count "pgrp pid=" 2 || fail "pipeline pgrp line missing"
type_word "pgrptest.elf pgrp | pgrptest.elf pgrp"
wait_for_count "pgrp pid=" 3 || fail "second pipeline pgrp line missing"
wait_for_count "> " 5 || fail "final prompt missing"

mapfile -t L < <(grep -o "pgrp pid=[0-9]* pgid=[0-9]*" "$SER")
[ "${#L[@]}" -eq 3 ] || fail "expected 3 pgrp lines, saw ${#L[@]}"
P1=$(echo "${L[0]}" | sed 's/pgrp pid=\([0-9]*\).*/\1/')
G1=$(echo "${L[0]}" | sed 's/.*pgid=\([0-9]*\)/\1/')
P2=$(echo "${L[1]}" | sed 's/pgrp pid=\([0-9]*\).*/\1/')
G2=$(echo "${L[1]}" | sed 's/.*pgid=\([0-9]*\)/\1/')
P3=$(echo "${L[2]}" | sed 's/pgrp pid=\([0-9]*\).*/\1/')
G3=$(echo "${L[2]}" | sed 's/.*pgid=\([0-9]*\)/\1/')
[ "$P1" != "$G1" ] || fail "exec child is its own group leader (pid $P1 == pgid $G1)"
[ "$G2" != "$P2" ] || fail "pipeline last stage leads its own group ($P2)"
[ "$G2" != "$G1" ] || fail "pipeline group is the shell group ($G2)"
[ "$G3" != "$P3" ] || fail "second pipeline last stage leads its own group ($P3)"
[ "$G3" != "$G1" ] || fail "second pipeline group is the shell group ($G3)"
[ "$G3" != "$G2" ] || fail "both pipelines share one group ($G2)"

echo "[test] PASS: pgid inheritance, setpgid rules, group kill; pipelines share a group"
exit 0
