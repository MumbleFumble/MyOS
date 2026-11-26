#!/usr/bin/env bash
# Phase 16 inc.1: shell variable store. Assignment lines
# (single NAME=value word), the set builtin (insertion-order
# listing), and unset. NO expansion yet. Asserted:
#  boot 1 (semantics): assignment is silent; set lists in
#    insertion order with empty values shown as NAME= and
#    values containing '=' kept whole; re-assignment replaces
#    in place (position kept); a builtin-named variable
#    (set=x) assigns, it does not dispatch; set with an
#    argument is a named error; NAME = value and 1X=2 keep
#    today's echo-fallback behaviour; X=1 | cat and X=1 > f
#    do NOT assign (pipeline / redirect lines); unset removes
#    (absent name and bad name are distinct named errors, no
#    argument is a usage line); a `run` script's assignments
#    persist into the interactive shell (same store); the
#    session's "> " count is exact, so silent lines and the
#    empty-store set print nothing at all.
#  boot 2 (limits): a 32-char name and a 128-char value are
#    named errors (31 / 127 are accepted); a failed
#    re-assignment leaves the old value intact; the 33rd
#    distinct variable is a named error while re-assigning
#    an existing one at capacity still works.
# The script fixture is injected host-side (its lines are
# data, never typed).
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
scripts = { 'envscript.txt': "SV=fromscript\nset\n" }
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
print("[test] injected envscript.txt into the image")
EOF

SER="$WORK/serial.log"
MON="$WORK/mon.sock"
QPID=""
cleanup() { [ -n "$QPID" ] && kill $QPID 2>/dev/null; [ -n "$QPID" ] && wait $QPID 2>/dev/null; }
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
check() { # check <serial> <string> <expected-count>
  local n
  n=$(count "$2" "$1")
  [ "$n" -eq "$3" ] || fail "count of '$2' is $n, expected $3"
}

boot() { # boot <image>
  rm -f "$SER" "$MON"
  qemu-system-x86_64 -m 64 -display none -serial "file:$SER" \
    -monitor "unix:$MON,server,nowait" \
    -drive file="$1",format=raw,if=ide,index=0 \
    -boot order=d,menu=off -cdrom build/myos.iso &
  QPID=$!
  for _ in $(seq 1 30); do [ -S "$MON" ] && break; sleep 1; done
  [ -S "$MON" ] || fail "monitor socket never appeared"
  wait_for_count "MyOS" 1 || fail "kernel banner missing"
  sleep 10
}
shutdown() { kill $QPID 2>/dev/null; wait $QPID 2>/dev/null; QPID=""; }

# ---------------- boot 1: semantics ----------------
boot "$WORK/disk.img"

type_word "set"                 # empty store: prints nothing
type_word "ALPHA=one"
type_word "BETA="
type_word "GAMMA=a=b"
type_word "set"
type_word "ALPHA=uno"           # replace in place
type_word "set"
type_word "set=x"               # a variable named like a builtin
type_word "set"
type_word "set foo"             # named error
type_word "NAME = value"        # not an assignment (spaces)
type_word "1X=2"                # not an assignment (bad name)
type_word "XPIPE=1 | cat"       # pipeline line: no assignment
type_word "XRED=1 > redir.txt"  # redirect line: no assignment
type_word "set"
type_word "unset BETA"
type_word "unset BETA"          # absent: named error
type_word "unset 1X"            # invalid name
type_word "unset"               # usage
type_word "unset ALPHA GAMMA"   # one arg with a space: invalid
type_word "run envscript.txt"   # script sets SV, lists store
type_word "set"                 # SV visible interactively

wait_for_count "SV=fromscript" 2 || fail "script-set variable not visible"
sleep 3

check "$SER" "ALPHA=one" 1
check "$SER" "ALPHA=uno" 5
check "$SER" "BETA=" 4
check "$SER" "GAMMA=a=b" 6
check "$SER" "set=x" 4
check "$SER" "SV=fromscript" 2
check "$SER" "set: takes no arguments" 1
check "$SER" "echo: NAME = value" 1
check "$SER" "echo: 1X=2" 1
check "$SER" "XPIPE=1" 1
check "$SER" "XRED=1" 0
check "$SER" "unset: no such variable: BETA" 1
check "$SER" "unset: invalid variable name" 2
check "$SER" "usage: unset <name>" 1
check "$SER" "> " 23
shutdown

# host check: redir.txt holds the echo fallback output
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
assert b'XRED=1' in live.get('redir.txt', b''), live.get('redir.txt')
print("[test] host: redir.txt holds the echo output, no assignment happened")
EOF

# ---------------- boot 2: limits and capacity ----------------
cp "$IMG" "$WORK/disk2.img"
boot "$WORK/disk2.img"

N32=$(printf 'A%.0s' $(seq 1 32))
N31=$(printf 'A%.0s' $(seq 1 31))
X128=$(printf 'x%.0s' $(seq 1 128))
Y127=$(printf 'y%.0s' $(seq 1 127))

type_word "$N32=v"        # name too long
type_word "$N31=v"        # longest legal name
type_word "BIG=ok"
type_word "BIG=$X128"     # value too long; BIG keeps ok
type_word "V=$Y127"       # longest legal value
type_word "set"
for i in $(seq -w 1 29); do type_word "C$i=x"; done   # store: 3 + 29 = 32
type_word "C30=x"         # 33rd distinct name: refused
type_word "BIG=replaced"  # re-assignment at capacity: fine
type_word "set"

wait_for_count "BIG=replaced" 1 || fail "re-assignment at capacity failed"
sleep 3

check "$SER" "shell: variable name too long" 1
check "$SER" "shell: variable value too long" 1
check "$SER" "shell: too many variables" 1
check "$SER" "BIG=ok" 1
check "$SER" "BIG=replaced" 1
check "$SER" "$Y127" 2
shutdown

echo "[test] PASS: assignment lines, set/unset, limits and script visibility"
