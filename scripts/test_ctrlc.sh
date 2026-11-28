#!/usr/bin/env bash
# Phase 18 inc.5: Ctrl-C delivers SIGINT to the
# foreground group through the REAL keyboard path
# (QEMU monitor sendkey ctrl-c -> PS/2 IRQ -> driver
# diversion -> pending record -> delivery task ->
# group kill). Cases: an exec'd spinner is
# interrupted (shell collects the signal death, ? = 1,
# shell unharmed); a two-stage external pipeline is
# interrupted (both stages die, "pipeline failed");
# a Ctrl-C at the idle prompt is a no-op that neither
# kills the shell, inserts a 'c', nor discards the
# partially typed line (hel + Ctrl-C + p runs help).
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

type_word "exec hostileprobe.elf spin"
sleep 2
ctrl_c
wait_for_count "> " 2 || fail "prompt did not return after Ctrl-C on exec spinner"
type_word "frobnicate s\$?"
wait_for_count "frobnicate s1" 1 || fail "Ctrl-C death did not leave ? = 1"

type_word "hostileprobe.elf spin | wc.elf"
sleep 2
ctrl_c
wait_for_count "shell: pipeline failed" 1 || fail "pipeline stages did not die on Ctrl-C"
wait_for_count "> " 4 || fail "prompt did not return after pipeline Ctrl-C"

type_chars "hel"
ctrl_c
type_chars "p"
send "sendkey ret"
sleep 0.6
wait_for_count "commands: ls" 1 || fail "help did not run after prompt-level Ctrl-C"
check "echo: hel" 0

check "frobnicate s1" 1
check "shell: pipeline failed" 1
# 5 real prompts (banner + one per typed line) plus one
# "> " inside the help text itself ("exec <prog> [").
check "> " 6

echo "[test] PASS: Ctrl-C interrupts exec + pipeline via the keyboard path; idle prompt is a safe no-op"
exit 0
