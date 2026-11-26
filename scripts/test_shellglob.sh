#!/usr/bin/env bash
# Phase 16 inc.5: globbing (pathname expansion), the final
# Phase 16 step. Order: parse -> expand -> glob -> dispatch.
# Eligibility is per-character (expand_str's mask): only
# pattern characters from unquoted text or unquoted
# expansion values glob. Matches are pinned SORTED (byte
# order) against the sys_readdir listing ls uses, over the
# flat MyFS directory.
#
# The GOLDEN block: gscript.gs runs first in a fresh
# store and its ENTIRE output is compared byte-for-byte
# with an expected block (the Phase 16 milestone's
# golden-output QEMU script test lives here). It pins in
# one shot: multi-match arg globbing in sorted order (via
# cat's whole-arg not-found message), a quoted pattern
# staying literal, assignment RHS staying literal (the set
# listing), an unquoted expansion globbing and a quoted
# one not.
#
# Interactive cases then pin: '?' and classes (incl.
# negation and an unterminated '[' staying literal), '**'
# behaving as '*', a '/'-pattern never matching the flat
# FS, single-match and zero-match (literal) redirect
# targets, a multi-match target as the ambiguous-redirect
# error with $? = 1 after it, command-word globbing (a
# file named "cat" makes c*t find the builtin name; a
# stage globbing to "exit" is refused), a globbed pipeline
# stage, the arg-overflow named error with no dispatch,
# and a script line globbing identically.
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
  'gscript.gs': 'cat g*.txt\ncat "g*.txt"\nV=g*.txt\ncat $V\ncat "$V"\nset\nfrobnicate end\n',
  'gscript2.gs': 'cat gold[2].txt\n',
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
print("[test] injected gscript.gs, gscript2.gs")
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
      '{') k="shift-bracket_left" ;;
      '}') k="shift-bracket_right" ;;
      "'") k="apostrophe" ;;
      '"') k="shift-apostrophe" ;;
      '*') k="shift-8" ;;
      '[') k="bracket_left" ;;
      ']') k="bracket_right" ;;
      '!') k="shift-1" ;;
      [A-Z]) k="shift-$(echo "$c" | tr 'A-Z' 'a-z')" ;;
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

# gold2 is written BEFORE gold1 on purpose: the directory
# listing order is then gold2, gold1, so the golden block's
# "gold1.txt gold2.txt" output also proves the matches are
# SORTED, not in listing order.
type_word "write gold2.txt gold two"
type_word "write gold1.txt gold one"
type_word "write cat dummy"
type_word "write exit dummy"
type_word "run gscript.gs"

wait_for_count "echo: frobnicate end" 1 || fail "golden script did not finish"
sleep 2

# Golden comparison: the script's entire output, byte-for-byte.
# Note the exact bytes: cat's not-found message carries no
# newline of its own (interactively the prompt's leading
# \r\n closes its line), so inside a script -- no prompts
# between lines -- consecutive messages run together. The
# set listing and the echo fallback do terminate with \r\n.
python3 - "$SER" << 'EOF' || fail "golden output mismatch"
import sys
data = open(sys.argv[1], 'rb').read()
expected = (b"cat: not found: gold1.txt gold2.txt"
            b"cat: not found: g*.txt"
            b"cat: not found: gold1.txt gold2.txt"
            b"cat: not found: g*.txt"
            b"V=g*.txt\r\n"
            b"echo: frobnicate end\r\n")
start = data.find(b"cat: not found: gold1.txt gold2.txt")
assert start >= 0, "golden start not found"
got = data[start:start + len(expected)]
assert got == expected, "golden mismatch:\n got: %r\nwant: %r" % (got, expected)
print("[test] golden output matches byte-for-byte")
EOF

type_word "cat gold?.txt"
type_word "cat gold[12].txt"
type_word "cat gold[!1].txt"
type_word "cat zzz*.txt"
type_word "cat 'g*.txt'"
type_word "cat a/*.txt"
type_word "cat g**.txt"
type_word "cat gold[.txt"
type_word "write gold3.bak three"
type_word "cat gold1.txt > gold3.ba?"
type_word "cat gold1.txt > new*.txt"
type_word "cat gold1.txt > g*.txt"
type_word "frobnicate m\$?"
type_word "c*t gold1.txt"
type_word "cat gold1.txt | ex*t"
type_word "cat gold[!2].txt | cat"
type_word "W2=*.txt"
type_word "set"
type_word "cat *"
type_word "run gscript2.gs"
type_word "frobnicate after\$?"

wait_for_count "echo: frobnicate after0" 1 || fail "final lines missing"
sleep 3

check "cat: not found: gold1.txt gold2.txt" 5
check "cat: not found: g*.txt" 3
check "V=g*.txt" 2
check "W2=*.txt" 1
check "echo: frobnicate end" 1
check "gold two" 2
check "gold one" 2
check "cat: not found: zzz*.txt" 1
check "cat: not found: a/*.txt" 1
check "cat: not found: gold[.txt" 1
check "shell: ambiguous redirect" 1
check "echo: frobnicate m1" 1
check "shell: exit not supported in pipelines" 1
check "shell: expansion too long" 1
check "echo: frobnicate after0" 1
check "> " 27

python3 - "$WORK/disk.img" << 'EOF' || fail "host walk failed"
import struct, sys
img = open(sys.argv[1], 'rb').read()
magic, count = struct.unpack_from('<II', img, 0)
assert magic == 0x4D594653
live = {}
off = 512
for _ in range(count):
    name = img[off:off+64].split(b'\0')[0].decode()
    size = struct.unpack_from('<Q', img, off + 64)[0]
    flag = img[off + 72]
    if flag == 0:
        live[name] = img[off + 80: off + 80 + size]
    off += ((80 + size + 511)//512)*512
assert live.get('gold3.bak') == b'gold one\n', live.get('gold3.bak')
assert live.get('new*.txt') == b'gold one\n', live.get('new*.txt')
assert live.get('gold1.txt') == b'gold one\n', live.get('gold1.txt')
assert live.get('gold2.txt') == b'gold two\n', live.get('gold2.txt')
print("[test] host: single-match target replaced, zero-match literal, ambiguous created nothing")
EOF

echo "[test] PASS: globbing per word with mask, sorted matches, golden output"
