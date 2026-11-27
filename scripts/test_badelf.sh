#!/usr/bin/env bash
# Phase 17 inc.4: ELF loader hardening. Four crafted
# malformed ELFs (host-side mutations of argtest.elf, see
# scripts/make_badelf.py) are spawned in turn; the loader
# must refuse each with -1 (surfaced by the shell as
# "exec: failed") before any out-of-bounds read or wild
# copy, the shell must stay alive after each attempt, and
# the unmutated program must still run correctly at the
# end. Before this increment each of these headers was
# trusted: the phdr table and segment copies read outside
# the file buffer and the entry point was never checked.
set -u
cd "$(dirname "$0")/.."
IMG=build/disk.img
[ -f "$IMG" ] || { echo "[test] FAIL: $IMG missing"; exit 1; }
[ -f build/user/argtest.elf ] || { echo "[test] FAIL: argtest.elf missing"; exit 1; }

WORK=$(mktemp -d)
cp "$IMG" "$WORK/disk.img"
python3 scripts/make_badelf.py build/user/argtest.elf "$WORK/disk.img" \
  || { echo "[test] FAIL: crafting failed"; exit 1; }

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

for n in 1 2 3 4; do
  type_word "exec badelf$n.elf"
  wait_for_count "exec: failed" "$n" || fail "badelf$n was not refused"
done

type_word "exec argtest.elf one two"
wait_for_count "argc=3" 1 || fail "valid program did not run after the refusals"

type_word "frobnicate alive"
wait_for_count "echo: frobnicate alive" 1 || fail "shell not dispatching afterwards"
sleep 2

check "exec: failed" 4
check "argc=3" 1
check "echo: frobnicate alive" 1
check "> " 7

echo "[test] PASS: malformed ELFs refused, shell alive, valid ELF still loads"
