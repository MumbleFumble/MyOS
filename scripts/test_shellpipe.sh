#!/usr/bin/env bash
# Phase 14 inc.6: pipelines execute. Builtins are in-process
# and NO builtin reads fd 0, so a pipeline's observable
# behaviour is: intermediate stages' output is captured by
# the pipe (never reaches the console), the last stage's own
# output reaches the console (or the redirect target)
# exactly once, fd 0 and fd 1 are restored (every command
# after a pipeline is read and answered normally — the whole
# sequence is the fd-0 proof), a producer overflowing the
# 1024-byte ring finishes without hanging, and the declared
# edge rules hold (exec/exit refused by name before anything
# runs; unknown stages echo into their wired stdout).
#
# Console ledger for "alpha beta gamma": case 1 (+1), the
# overflow case (+1), case 5 (+1), the 8x leak loop (+8),
# the final plain cat (+1) = 12. "red blue" appears once
# (the three-stage pipeline's last stage).
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

for _ in $(seq 1 30); do [ -S "$MON" ] && break; sleep 1; done
[ -S "$MON" ] || fail "monitor socket never appeared"
wait_for_count "MyOS" 1 || fail "kernel banner missing"
sleep 10

L="lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna"
type_word "write words.txt alpha beta gamma"
type_word "write other.txt red blue"
type_word "cat other.txt | cat words.txt"
type_word "cat other.txt | cat words.txt | cat other.txt"
type_word "write big.txt $L"
for i in 1 2 3 4 5 6 7 8; do type_word "append big.txt $L"; done
type_word "cat big.txt | cat words.txt"
type_word "cat other.txt | cat words.txt > final.txt"
type_word "frobnicate x | cat words.txt"
type_word "cat words.txt | frobnicate x"
type_word "cat words.txt | exec argtest.elf"
type_word "ls | exit"
for i in 1 2 3 4 5 6 7 8; do type_word "ls | cat words.txt"; done
type_word "cat words.txt > after.txt"
type_word "cat after.txt"

wait_for_count "alpha beta gamma" 12 || fail "sequence did not complete"
sleep 2

[ "$(count 'alpha beta gamma' "$SER")" -eq 12 ] || fail "alpha count != 12"
[ "$(count 'red blue' "$SER")" -eq 1 ] || fail "red blue count != 1"
[ "$(count 'echo: frobnicate x' "$SER")" -eq 1 ] || fail "unknown last stage did not echo to console"
[ "$(count 'shell: exec not supported in pipelines' "$SER")" -eq 1 ] || fail "exec refusal missing"
[ "$(count 'shell: exit not supported in pipelines' "$SER")" -eq 1 ] || fail "exit refusal missing"
[ "$(count 'argc=' "$SER")" -eq 0 ] || fail "exec stage ran despite refusal"
[ "$(count 'appended' "$SER")" -eq 8 ] || fail "big.txt append ledger != 8"

python3 - "$WORK/disk.img" << 'EOF' || fail "host-side image check failed"
import struct, sys
img = open(sys.argv[1], 'rb').read()
total = len(img) // 512
magic, count = struct.unpack_from('<II', img, 0)
assert magic == 0x4D594653, hex(magic)
limit = total - 2048 if total > 3072 else total
off = 512
live = {}
for _ in range(count):
    hdr = img[off:off+80]
    name = hdr[:64].split(b'\0')[0].decode()
    size = struct.unpack_from('<Q', hdr, 64)[0]
    deleted = hdr[72] == 1
    data = img[off+80:off+80+size]
    if not deleted:
        live[name] = data
    off += ((80 + size + 511)//512)*512
assert live.get('words.txt') == b'alpha beta gamma\n', live.get('words.txt')
assert live.get('other.txt') == b'red blue\n', live.get('other.txt')
assert live.get('final.txt') == b'alpha beta gamma\n', live.get('final.txt')
assert live.get('after.txt') == b'alpha beta gamma\n', live.get('after.txt')
assert len(live.get('big.txt', b'')) > 1024, len(live.get('big.txt', b''))
assert live['big.txt'].startswith(b'lorem ipsum'), live['big.txt'][:32]
print("[test] host-side image check: pipeline+redirect target exact, "
      "post-pipeline redirect exact, big.txt producer file is %d bytes "
      "(> 1024 ring)" % len(live['big.txt']))
EOF

echo "[test] PASS: pipelines execute with console-exactly-once output, ring overflow without hang, named exec/exit refusals, and fd 0/1 restore"
