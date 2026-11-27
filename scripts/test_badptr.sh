#!/usr/bin/env bash
# Phase 17 inc.3: copy_from/to_user + EFAULT. A ring-3
# program passes bad pointers (shared PML4[0] kernel range,
# unmapped user range, a range straddling a mapped/unmapped
# page boundary) to the core syscalls; every call must be
# refused with EFAULT (-3), the program must keep running,
# a valid round-trip must succeed byte-exact, and the shell
# must still dispatch afterwards. Before this increment the
# kernel-range write corrupted kernel memory and the
# unmapped pointer halted the machine at CPL0.
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

type_word "exec badptr.elf"
wait_for_count "badptr: ALL PASS" 1 || fail "badptr did not pass"

type_word "exec argtest.elf one two"
wait_for_count "argc=3" 1 || fail "argtest did not run afterwards"

type_word "frobnicate alive"
wait_for_count "echo: frobnicate alive" 1 || fail "shell not dispatching afterwards"
sleep 2

check "badptr: ok " 10
check "badptr: FAIL" 0
check "badptr: pid stable" 1
check "badptr: roundtrip OK" 1
check "argc=3" 1
check "echo: frobnicate alive" 1
check "> " 4

echo "[test] PASS: bad pointers refused with EFAULT, task and kernel alive"
