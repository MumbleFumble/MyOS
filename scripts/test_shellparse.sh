#!/usr/bin/env bash
# Headless test for Phase 14 inc.4: shell parser.
#
# Drives the shell on a COPY of the disk image and asserts:
#  - a multi-word write/append round-trip is byte-identical
#    (checked host-side against the image afterwards);
#  - collapsed separators now work: "cat  words.txt" (two
#    spaces) reads the file (the old first-space split handed
#    arg a leading space and the open failed);
#  - "cat words.txt|cat" prints the pipelines not-yet-supported
#    message and executes nothing; "cat words.txt > out.txt"
#    EXECUTES since Phase 14 inc.5 (out.txt is created with the
#    file's contents; positive coverage in test_shellred.sh);
#  - "| cat", "cat words.txt >" and "cat words.txt > a > b"
#    print their named parse errors;
#  - an ordinary command still runs after error lines, the
#    echo fallback for unknown commands is unchanged, and a
#    trailing space still reaches arg ("cat words.txt "
#    fails exactly as before the parser).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
TMP="$(mktemp -d /tmp/myos_parse.XXXXXX)"

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }
[ -f "$ROOT/build/disk.img" ] || { echo "[test] FAIL: build/disk.img missing"; exit 1; }
command -v qemu-system-x86_64 >/dev/null || { echo "[test] FAIL: qemu-system-x86_64 missing"; exit 1; }

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1

IMG="$TMP/disk.img"
cp "$ROOT/build/disk.img" "$IMG"
SER="$TMP/serial.txt"; SOCK="$TMP/mon.sock"
rm -f "$SER" "$SOCK"
qemu-system-x86_64 -m 256 -display none -vga none \
  -serial "file:$SER" -monitor "unix:$SOCK,server,nowait" \
  -boot d -cdrom "$ISO" -drive "file=$IMG,format=raw,if=ide,index=0" \
  -netdev "socket,id=n0,listen=127.0.0.1:10000" \
  -device "e1000,netdev=n0,mac=52:54:00:12:34:56" &
QPID=$!
cleanup() { kill -9 "$QPID" 2>/dev/null; rm -rf "$TMP"; }
fail() { echo "[test] FAIL: $1"; cleanup; exit 1; }

for i in $(seq 1 40); do [ -S "$SOCK" ] && break; sleep 0.5; done
sleep 20

type_word() {
  local str="$1" i c
  for (( i=0; i<${#str}; i++ )); do
    c="${str:$i:1}"
    case "$c" in
      ' ') echo "sendkey spc" ;;
      '.') echo "sendkey dot" ;;
      '_') echo "sendkey shift-minus" ;;
      '-') echo "sendkey minus" ;;
      '>') echo "sendkey shift-dot" ;;
      '|') echo "sendkey shift-backslash" ;;
      *)   echo "sendkey $c" ;;
    esac
    sleep 0.06
  done
  echo "sendkey ret"
  sleep 0.6
}
{
  type_word "write words.txt alpha beta gamma"
  type_word "cat words.txt"
  type_word "append words.txt delta epsilon"
  type_word "cat words.txt"
  type_word "cat  words.txt"
  type_word "cat words.txt|cat"
  type_word "cat words.txt > out.txt"
  type_word "ls"
  type_word "cat out.txt"
  type_word "| cat"
  type_word "cat words.txt >"
  type_word "cat words.txt > a > b"
  type_word "cat words.txt"
  type_word "frobnicate hello world"
  type_word "cat words.txt "
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

count() { grep -a -c "$1" "$2" 2>/dev/null || true; }
for i in $(seq 1 60); do
  [ "$(count 'echo: frobnicate hello world' "$SER")" -ge 1 ] && break
  sleep 1
done
sleep 2

[ "$(count 'alpha beta gamma' "$SER")" -ge 3 ] || fail "contents missing (plain/collapsed/after-error cats)"
[ "$(count 'delta epsilon' "$SER")" -ge 1 ] || fail "appended line missing"
[ "$(count 'not found:  words' "$SER")" -eq 0 ] || fail "double-space arg leak still present"
[ "$(count 'shell: pipelines not yet supported' "$SER")" -eq 1 ] || fail "pipelines not-yet-supported count != 1"
[ "$(count 'shell: parse error: empty command in pipeline' "$SER")" -eq 1 ] || fail "empty-stage error missing"
[ "$(count 'shell: parse error: missing redirect target' "$SER")" -eq 1 ] || fail "missing-target error missing"
[ "$(count 'shell: parse error: multiple redirections' "$SER")" -eq 1 ] || fail "multiple-redirections error missing"
[ "$(count 'cat: not found: out.txt' "$SER")" -eq 0 ] || fail "cat out.txt failed - redirection did not create it"
[ "$(count 'out.txt' "$SER")" -eq 1 ] || fail "out.txt missing from the ls listing - redirection did not execute?"
[ "$(count 'echo: frobnicate hello world' "$SER")" -eq 1 ] || fail "echo fallback changed"
[ "$(count 'cat: not found: words.txt ' "$SER")" -eq 1 ] || fail "trailing space no longer reaches arg"

python3 - "$IMG" << 'PYEOF' || fail "words.txt bytes differ from the pre-parser round-trip"
import struct, sys
img = open(sys.argv[1], 'rb').read()
count = struct.unpack_from('<I', img, 4)[0]
off = 512
live = {}
for _ in range(count):
    name = img[off:off+64].split(b'\0')[0].decode()
    size = struct.unpack_from('<Q', img, off + 64)[0]
    flag = img[off + 72]
    if flag == 0:
        live[name] = img[off + 80: off + 80 + size]
    off += ((80 + size + 511) // 512) * 512
assert live.get('words.txt') == b'alpha beta gamma\ndelta epsilon\n', live.get('words.txt')
assert live.get('out.txt') == b'alpha beta gamma\ndelta epsilon\n', live.get('out.txt')
assert 'a' not in live and 'b' not in live, list(live)
print("[test] host-side image check: words.txt byte-identical, out.txt created by redirection, no a/b created")
PYEOF

echo "[test] PASS: parser preserves plain commands, rejects operators honestly, names parse errors"
cleanup
exit 0
