#!/usr/bin/env bash
# Phase 14 inc.7: bare cat is a stdin filter. With fd 0 wired
# to a pipe (a pipeline stage after the first) it copies fd 0
# to fd 1 until EOF, so pipelines are true pass-throughs.
# With fd 0 on the console it keeps the old usage behaviour,
# so interactive bare cat can never hang the shell.
#
# Console ledger for "alpha beta gamma": two-stage (+1),
# three-stage (+1), first-stage-bare-cat pipeline (+1: the
# usage goes into the pipe and is discarded, the file stage
# prints), 20x leak loop (+20), final plain cat (+1) = 24.
# "usage: cat <file>" appears exactly once: the final
# interactive bare cat, proving the wiring flag reset.
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
  for _ in $(seq 1 150); do
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
type_word "cat words.txt | cat"
type_word "cat words.txt | cat | cat"
type_word "cat words.txt | cat > final.txt"
type_word "cat | cat words.txt"
type_word "cat missing.txt | cat"
type_word "write big.txt $L"
for i in 1 2 3 4 5 6 7 8; do type_word "append big.txt $L"; done
type_word "cat big.txt | cat"
for i in $(seq 1 20); do type_word "cat words.txt | cat"; done
type_word "cat words.txt"
type_word "cat"

wait_for_count "alpha beta gamma" 24 || fail "sequence did not complete"
sleep 2

[ "$(count 'alpha beta gamma' "$SER")" -eq 24 ] || fail "alpha count != 24"
[ "$(count 'usage: cat <file>' "$SER")" -eq 1 ] || fail "usage count != 1 (only the interactive bare cat)"
[ "$(count 'cat: not found: missing.txt' "$SER")" -eq 1 ] || fail "upstream error text did not pass through as data"
[ "$(count 'lorem ipsum' "$SER")" -eq 9 ] || fail "overflow pass-through: lorem count != 9"
[ "$(count 'magna' "$SER")" -eq 8 ] || fail "overflow pass-through: magna count != 8"
[ "$(count 'stdin read error' "$SER")" -eq 0 ] || fail "a negative stdin read was reported (writer leak?)"
[ "$(count 'pipeline failed' "$SER")" -eq 0 ] || fail "pipeline failed during the leak loop"
[ "$(count 'appended' "$SER")" -eq 8 ] || fail "big.txt append ledger != 8"

python3 - "$WORK/disk.img" << 'EOF' || fail "host-side image check failed"
import struct, sys
img = open(sys.argv[1], 'rb').read()
total = len(img) // 512
magic, count = struct.unpack_from('<II', img, 0)
assert magic == 0x4D594653, hex(magic)
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
assert live.get('final.txt') == b'alpha beta gamma\n', live.get('final.txt')
assert len(live.get('big.txt', b'')) > 1024, len(live.get('big.txt', b''))
print("[test] host-side image check: pipeline-into-redirect holds the "
      "passed-through data exactly; big.txt is %d bytes (> 1024 ring)"
      % len(live['big.txt']))
EOF

echo "[test] PASS: bare cat passes pipelines through exactly once (2- and 3-stage), errors flow as data, ring overflow truncates without hanging, 20x leak loop clean, interactive bare cat still prints usage"
