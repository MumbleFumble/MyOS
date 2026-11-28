#!/usr/bin/env bash
# Phase 18 inc.2: SYS_KILL + fd teardown on death.
# killtest.elf orchestrates: a killed spinner collects
# exactly 4105 (EXIT_SIGNAL_BASE + SIGKILL); bad-pid and already-DEAD kills return
# -1 with the dead status unchanged; kill(self)
# collects 4105; a killed pipe writer delivers EOF
# only at its death (teardown), never before; a task
# blocked in SYS_WAIT is killed, collects 4105 and
# never resumes (its resurrection marker must appear
# 0 times). Fixtures reused unmodified: hostileprobe
# (spinner), fdchild (inherited writer), argtest.
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

type_word "exec killtest.elf"
wait_for_count "killtest: ALL PASS" 1 || fail "killtest did not reach ALL PASS"

check "killtest: ok " 16
check "killtest: FAIL" 0
check "WAITER-RESURRECTED" 0
check "killtest: ALL PASS" 1

type_word "frobnicate k1"
wait_for_count "frobnicate k1" 1 || fail "shell not alive after killtest"
check "> " 3

echo "[test] PASS: SYS_KILL collects 4105, EOF lands on kill, no resurrection"
exit 0
