#!/usr/bin/env bash
# Phase 18 inc.6: background jobs. A trailing & on an
# external line launches it in its own process group
# with a job notice and an immediate prompt; jobs
# lists it while a following builtin runs with the
# spinner still alive; fg + Ctrl-C through the real
# keyboard path kills the foregrounded job (inc-003/
# 005 semantics, ? = 1, shell spared); a job that
# fails on its own is reported at a prompt as
# "[n] Exit <code>" WITHOUT overwriting the
# foreground ?; the misuse cases refuse by name; the
# fixed-size table refuses a 9th concurrent job.
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

type_chars() { # no Enter
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
      '%') k="shift-5" ;;
      '&') k="shift-7" ;;
      '?') k="shift-slash" ;;
      '{') k="shift-bracket_left" ;;
      '}') k="shift-bracket_right" ;;
      [A-Z]) k="shift-$(echo "$c" | tr 'A-Z' 'a-z')" ;;
      *)   k="$c" ;;
    esac
    send "sendkey $k"
    sleep 0.06
  done
}
type_word() { type_chars "$1"; send "sendkey ret"; sleep 0.6; }
ctrl_c() { send "sendkey ctrl-c"; sleep 0.3; }

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

# fg with no jobs: named error, shell alive.
type_word "fg"
wait_for_count "fg: no current job" 1 || fail "fg with no jobs not refused by name"

# Background spinner: notice, immediate prompt.
type_word "hostileprobe.elf spin &"
wait_for_count "[1] " 1 || fail "no job notice for background launch"
PID1=$(grep -a -o '\[1\] [0-9][0-9]*' "$SER" | tail -1 | sed 's/.*] //')
[ -n "$PID1" ] || fail "could not read job pid from the notice"

# jobs (a builtin running while the spinner lives)
# lists it in its own group (pgid == its pid).
type_word "jobs"
wait_for_count "Running pgid=$PID1 hostileprobe.elf spin" 1 \
  || fail "jobs does not list the spinner in its own group"

# fg + Ctrl-C through the real keyboard path.
type_word "fg %1"
sleep 1
ctrl_c
wait_for_count "hostileprobe.elf spin" 2 || fail "fg did not foreground the job"
type_word "frobnicate s\$?"
wait_for_count "frobnicate s1" 1 || fail "Ctrl-C death via fg did not leave ? = 1"

# A job that fails on its own: reported at a prompt
# as Exit 4111, and its status must NOT overwrite
# the foreground ? (the launch line left ? = 0).
type_word "signaltest.elf selfterm &"
wait_for_count "[2] " 1 || fail "no job notice for the selfterm launch"
sleep 2
type_word "frobnicate s\$?"
wait_for_count "frobnicate s0" 1 || fail "background completion overwrote the foreground ?"
wait_for_count "[2] Exit 4111 signaltest.elf selfterm" 1 \
  || fail "finished job not reported/reaped at a prompt"

# Misuse cases refuse by name.
type_word "hostileprobe.elf spin & extra"
wait_for_count "shell: '&' must end the line" 1 || fail "mid-line & not refused by name"
type_word "help &"
wait_for_count "shell: & requires an external command" 1 \
  || fail "builtin with & not refused by name"

# Table capacity: 8 running jobs fill it; the 9th is
# refused before spawning. Then fg + Ctrl-C reaps
# each in turn (bare fg takes the most recent job).
for i in 1 2 3 4 5 6 7 8; do
  type_word "hostileprobe.elf spin &"
done
wait_for_count "[10] " 1 || fail "job notices for the table fill missing"
type_word "hostileprobe.elf spin &"
wait_for_count "shell: job table full" 1 || fail "9th job not refused by name"
for i in 1 2 3 4 5 6 7 8; do
  type_word "fg"
  sleep 1
  ctrl_c
  sleep 2
done
type_word "frobnicate s\$?"
wait_for_count "frobnicate s1" 2 || fail "shell not alive/clean after the fg cleanup"
type_word "jobs"
sleep 1

check "fg: no current job" 1
check "Running" 1
check "hostileprobe.elf spin" 10
check "[2] Exit 4111 signaltest.elf selfterm" 1
check "shell: '&' must end the line" 1
check "shell: & requires an external command" 1
check "shell: job table full" 1
check "frobnicate s0" 1
check "frobnicate s1" 2

echo "[test] PASS: background jobs (&, jobs, fg) with own groups, prompt reaping, and the fg + Ctrl-C composition"
exit 0
