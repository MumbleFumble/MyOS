#!/usr/bin/env bash
# Phase 14 inc.8: shell scripts. A `run <file>` builtin
# executes file lines through the same parse/dispatch path
# as interactive lines. Stated semantics asserted here:
# no per-line prompts (the whole session's "> " count is
# exact), errors print and the script CONTINUES, exit in a
# script is refused by name and the session survives,
# nested run works and is depth-capped, an over-long line
# is skipped with a message, a '#' line is an ordinary
# unknown command (no comment syntax), and one prompt with
# the last effective line's flavour closes the script.
#
# The script fixtures are injected into the disk image
# host-side (appending MyFS records): their lines contain
# '|' and '>', which the interactive parser would intercept
# if they were typed through `write`.
set -u
cd "$(dirname "$0")/.."
IMG=build/disk.img
[ -f "$IMG" ] || { echo "[test] FAIL: $IMG missing"; exit 1; }

WORK=$(mktemp -d)
cp "$IMG" "$WORK/disk.img"

python3 - "$WORK/disk.img" << 'EOF' || { echo "[test] FAIL: fixture injection failed"; exit 1; }
import struct, sys
path = sys.argv[1]
img = bytearray(open(path, 'rb').read())
total = len(img) // 512
magic, count = struct.unpack_from('<II', img, 0)
assert magic == 0x4D594653, hex(magic)
limit = (total - 2048) * 512 if total > 3072 else total * 512
off = 512
for _ in range(count):
    size = struct.unpack_from('<Q', img, off + 64)[0]
    off += ((80 + size + 511)//512)*512
scripts = {
 's.txt': ("cat words.txt\n"
           "cat words.txt | cat\n"
           "cat words.txt | cat > final.txt\n"
           "cat words.txt | cat\n"
           "frobnicate mid\n"
           "cat |\n"
           "exit\n"
           "run nope.txt\n"
           "run inner.txt\n"
           "cat words.txt\n"
           "# comment line\n"
           "cat words.txt\n"),
 'inner.txt': "cat words.txt\n",
 'loop.txt': "run loop.txt\n",
 'long.txt': "frobnicate " + "x"*289 + "\ncat words.txt\n",
}
for name, text in scripts.items():
    data = text.encode()
    hdr = bytearray(80)
    nb = name.encode()
    hdr[:len(nb)] = nb
    struct.pack_into('<Q', hdr, 64, len(data))
    rec = bytes(hdr) + data
    rec += b'\0' * (((len(rec) + 511)//512)*512 - len(rec))
    assert off + len(rec) <= limit, "image full"
    img[off:off+len(rec)] = rec
    off += len(rec)
    count += 1
struct.pack_into('<I', img, 4, count)
open(path, 'wb').write(bytes(img))
print("[test] injected s.txt, inner.txt, loop.txt, long.txt into the image")
EOF

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

type_word "write words.txt alpha beta gamma"
type_word "run s.txt"
type_word "cat words.txt"
type_word "run missing.txt"
type_word "cat"
type_word "run loop.txt"
type_word "run long.txt"
type_word "cat words.txt"

wait_for_count "alpha beta gamma" 9 || fail "sequence did not complete"
sleep 2

[ "$(count 'alpha beta gamma' "$SER")" -eq 9 ] || fail "alpha count != 9"
[ "$(count '> ' "$SER")" -eq 9 ] || fail "prompt count != 9 (per-line prompts leaked?)"
[ "$(count 'echo: frobnicate mid' "$SER")" -eq 1 ] || fail "unknown line in script did not echo once"
[ "$(count 'echo: frobnicate' "$SER")" -eq 1 ] || fail "over-long line executed instead of being skipped"
[ "$(count 'shell: parse error: empty command in pipeline' "$SER")" -eq 1 ] || fail "parse error in script missing"
[ "$(count 'run: exit not allowed in a script' "$SER")" -eq 1 ] || fail "exit refusal missing"
[ "$(count 'run: not found: nope.txt' "$SER")" -eq 1 ] || fail "nested missing-file error missing"
[ "$(count 'run: not found: missing.txt' "$SER")" -eq 1 ] || fail "run <missing> error missing"
[ "$(count 'echo: # comment line' "$SER")" -eq 1 ] || fail "# line was not treated as an ordinary unknown command"
[ "$(count 'usage: cat <file>' "$SER")" -eq 1 ] || fail "interactive bare cat after scripts broken"
[ "$(count 'run: nesting too deep' "$SER")" -eq 1 ] || fail "nesting cap message missing"
[ "$(count 'run: line too long, skipped' "$SER")" -eq 1 ] || fail "line-too-long skip missing"

python3 - "$WORK/disk.img" << 'EOF' || fail "host-side image check failed"
import struct, sys
img = open(sys.argv[1], 'rb').read()
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
print("[test] host-side image check: script-internal pipeline+redirect "
      "target byte-exact")
EOF

echo "[test] PASS: scripts run plain/pipeline/redirect lines with no per-line prompts, continue on errors, refuse exit, cap nesting, skip over-long lines, and leave the session normal"
