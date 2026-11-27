#!/usr/bin/env bash
# Phase 17 inc.2: kill-the-task fault path. A faulting USER
# task dies with the distinctive exit code EXIT_FAULT
# (4000), collected by its parent through SYS_WAIT, while
# the kernel, the shell and other tasks run on. Before this
# increment the same faults halted the whole kernel (#PF)
# or triple-faulted the machine (#DE/#UD/#GP had no gates).
#
# faulttest.elf is the waiter harness: it spawns
# faultprobe.elf <mode>, waits, and prints the collected
# code plus a PASS/FAIL verdict (it is also the first
# program in the tree to collect a NONZERO exit code
# through SYS_WAIT; the sched-exit path used to zero it).
# Modes: null (#PF cr2=0), div0 (#DE), ud2 (#UD), gp (#GP
# via hlt at ring 3), ss (#SS via non-canonical RSP). The
# "ok" control exits 0. After all modes, argtest runs as an
# unrelated known-good program and a fallback line proves
# the shell is still dispatching.
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

for mode in null div0 ud2 gp ss; do
  type_word "exec faulttest.elf $mode"
  wait_for_count "faulttest: PASS $mode" 1 || fail "mode $mode did not PASS"
done

type_word "exec faulttest.elf ok"
wait_for_count "faulttest: PASS ok" 1 || fail "control mode ok did not PASS"

type_word "exec argtest.elf one two"
wait_for_count "argc=3" 1 || fail "argtest did not run after the kills"

type_word "frobnicate alive"
wait_for_count "echo: frobnicate alive" 1 || fail "shell not dispatching after the kills"
sleep 2

check "exit=4000" 5
check "faulttest: PASS" 6
check "faultprobe: SURVIVED" 0
check "killing faulting task" 5
check "argc=3" 1
check "echo: frobnicate alive" 1
check "> " 9

echo "[test] PASS: faulting tasks killed with EXIT_FAULT, system alive"
