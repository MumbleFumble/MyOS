#!/usr/bin/env bash
# Phase 17 inc.7: shell pipelines of *.elf stages run as
# real ring-3 children through SYS_EXEC_INHERIT.
# At the prompt: cat.elf <file> | wc.elf prints wc's
# exact line for an empty file (0 0 0) and a 2000-byte
# file (200 400 2000, crossing the 1024-byte ring through
# concurrent stages); a three-stage external pipeline
# works; redirection composes on the last stage; a
# failing stage reports "shell: pipeline failed", sets
# $? nonzero, and a following valid pipeline still works;
# a mixed builtin/external pipeline is refused whole by
# name before anything runs (no redirect target created).
set -u
cd "$(dirname "$0")/.."
IMG=build/disk.img
[ -f "$IMG" ] || { echo "[test] FAIL: $IMG missing"; exit 1; }

WORK=$(mktemp -d)
cp "$IMG" "$WORK/disk.img"

# Host-side fixture injection into the test's copy:
# ep_empty.txt (0 bytes) and ep_big.txt (200 lines of
# "word word\n" = 2000 bytes), appended as MyFS records
# (the test_shellscript.sh pattern).
python3 - "$WORK/disk.img" << 'EOF'
import struct, sys
path = sys.argv[1]
img = bytearray(open(path, 'rb').read())
magic, count = struct.unpack_from('<II', img, 0)
assert magic == 0x4D594653, "bad magic"
off = 512
for _ in range(count):
    size = struct.unpack_from('<Q', img, off + 64)[0]
    off += ((80 + size + 511) // 512) * 512
def add(name, data):
    global off, count
    hdr = bytearray(80)
    nb = name.encode()
    hdr[0:len(nb)] = nb
    struct.pack_into('<Q', hdr, 64, len(data))
    rec = bytes(hdr) + data
    rec += b'\0' * (((len(rec) + 511) // 512) * 512 - len(rec))
    img[off:off + len(rec)] = rec
    off += len(rec)
    count += 1
add("ep_empty.txt", b"")
add("ep_big.txt", b"word word\n" * 200)
struct.pack_into('<I', img, 4, count)
open(path, 'wb').write(img)
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
      '=') k="equal" ;;
      '_') k="shift-minus" ;;
      '$') k="shift-4" ;;
      '?') k="shift-slash" ;;
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

type_word "cat.elf ep_big.txt | wc.elf"
wait_for_count "200 400 2000" 1 || fail "big pipeline did not print wc line"

type_word "cat.elf ep_empty.txt | wc.elf"
wait_for_count "0 0 0" 1 || fail "empty pipeline did not print 0 0 0"

type_word "cat.elf ep_big.txt | cat.elf | wc.elf"
wait_for_count "200 400 2000" 2 || fail "three-stage pipeline failed"

type_word "cat.elf missing.txt | wc.elf"
wait_for_count "shell: pipeline failed" 1 || fail "missing file not reported"

type_word "frobnicate st\$?"
wait_for_count "echo: frobnicate st1" 1 || fail "status after failed pipeline not 1"

type_word "cat.elf ep_big.txt | wc.elf"
wait_for_count "200 400 2000" 3 || fail "valid pipeline after failure broken"

type_word "cat.elf ep_big.txt | cat"
wait_for_count "mixed builtin and external" 1 || fail "mixed pipeline not refused"

type_word "cat.elf ep_big.txt | cat > mixtgt.txt"
wait_for_count "mixed builtin and external" 2 || fail "mixed redirect pipeline not refused"

type_word "cat.elf ep_big.txt | wc.elf > wc_out.txt"
sleep 3

type_word "cat wc_out.txt"
wait_for_count "200 400 2000" 4 || fail "redirected pipeline output wrong"

type_word "nosuch.elf | wc.elf"
wait_for_count "shell: pipeline failed" 2 || fail "nonexistent elf not reported"

type_word "exec argtest.elf one two"
wait_for_count "argc=3" 1 || fail "argtest did not run after pipelines"

type_word "frobnicate alive"
wait_for_count "echo: frobnicate alive" 1 || fail "shell not dispatching afterwards"
sleep 2

check "200 400 2000" 4
check "0 0 0" 2
check "shell: pipeline failed" 2
check "mixed builtin and external" 2
check "echo: frobnicate st1" 1
check "argc=3" 1
check "echo: frobnicate alive" 1
check "> " 14

# The refused mixed pipeline must not have created its
# redirect target; the composed one must exist with the
# wc line as its content.
python3 - "$WORK/disk.img" << 'EOF' || fail "host walk: redirect accounting wrong"
import struct, sys
img = open(sys.argv[1], 'rb').read()
magic, count = struct.unpack_from('<II', img, 0)
off = 512
live = {}
for _ in range(count):
    name = img[off:off+64].split(b'\0')[0].decode()
    size = struct.unpack_from('<Q', img, off + 64)[0]
    flag = img[off + 72]
    if not (flag & 1):
        live[name] = img[off + 80:off + 80 + size]
    off += ((80 + size + 511) // 512) * 512
assert "mixtgt.txt" not in live, "mixtgt.txt was created by a refused pipeline"
assert live.get("wc_out.txt") == b"200 400 2000\n", "wc_out.txt content wrong"
EOF

echo "[test] PASS: ELF pipeline stages at the prompt (exact counts, 3-stage, redirect, failure honesty, mixed refusal)"
