#!/usr/bin/env bash
# Phase 16 inc.2: variable expansion. $NAME / ${NAME} expand
# post-parse, per word, from the inc.1 store. Asserted:
# plain/braced/concatenated expansion; undefined -> empty
# (bracket case, and the empty word keeps its place: the
# double space in cat's not-found message); literal-$ cases
# ($, $1, $-, $@, $$ pass through untouched; the $? in that
# line expands since inc.3, to 0 there: the previous line's
# cat: not found is a builtin message, which the line-level
# status model counts as success); expanded text is never
# re-scanned (a stored "$INNER" stays literal);
# assignment RHS expands against the current store, a failed
# RHS stores nothing; pipeline stages and the redirect target
# expand; a bad substitution anywhere dispatches NOTHING (a
# pipeline's first stage does not run, a redirect target is
# never created); overflow is a named error, never a cut
# reference (the 63-char cap straddle); an expanded empty
# command / redirect target are named errors; a stage that
# expands to a session command is refused by its expanded
# name; script lines expand identically and a script
# assignment using expansion persists.
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
scripts = { 'expscript.txt': "cat $F\nS2=$W\nset\n" }
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
print("[test] injected expscript.txt into the image")
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
      '@') k="shift-2" ;;
      '{') k="shift-bracket_left" ;;
      '}') k="shift-bracket_right" ;;
      '[') k="bracket_left" ;;
      ']') k="bracket_right" ;;
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

X127=$(printf 'x%.0s' $(seq 1 127))
Y100=$(printf 'y%.0s' $(seq 1 100))
X59=$(printf 'x%.0s' $(seq 1 59))

type_word "write words.txt alpha beta gamma"
type_word "F=words.txt"
type_word "W=world"
type_word "INNER=world"
type_word "cat \$F"
type_word "cat \${F}"
type_word "frobnicate pre\${W}post"
type_word "frobnicate [\$UNDEF]"
type_word "cat pre \$UNDEF post"
type_word "frobnicate \$ \$1 \$- \$? \$@ \$\$"
type_word "X=\$W"
type_word "Y=pre\${W}post"
type_word "A=\$"
type_word "B=\${A}INNER"
type_word "frobnicate \$B"
type_word "Z=\${W"
type_word "cat \$F | cat"
type_word "cat \$F | cat \${F"
type_word "OUT=result.txt"
type_word "cat \$F > \$OUT"
type_word "cat \${F > nope.txt"
type_word "BIG=$X127"
type_word "frobnicate \$BIG\$BIG"
type_word "\$BIG"
type_word "A2=x\$BIG\${"
type_word "A3=pre\$BIG"
type_word "LONGNAME=0123456789"
type_word "${X59}\$LONGNAME"
type_word "V=$Y100"
type_word "frobnicate \$V | cat"
type_word "\$UNDEF foo"
type_word "\$UNDEF"
type_word "cat \$F > \$UNDEF"
type_word "XE=exit"
type_word "cat \$F | \$XE"
type_word "run expscript.txt"
type_word "set"

wait_for_count "S2=world" 2 || fail "script expansion/assignment missing"
sleep 3

check "alpha beta gamma" 4
check "echo: frobnicate preworldpost" 1
check "echo: frobnicate []" 1
check "cat: not found: pre  post" 1
check "echo: frobnicate \$ \$1 \$- 0 \$@ \$\$" 1
check "echo: frobnicate \$INNER" 1
check "echo: frobnicate" 5
check "shell: bad substitution" 4
check "shell: expansion too long" 3
check "shell: variable value too long" 1
check "shell: empty command" 2
check "shell: empty redirect target" 1
check "shell: exit not supported in pipelines" 1
check "$X127" 2
check "$Y100" 3
check "X=world" 2
check "Y=preworldpost" 2
check "B=\$INNER" 2
check "S2=world" 2
check "F=words.txt" 2
check "Z=" 0
check "A2=" 0
check "A3=" 0
check "> " 38

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
assert live.get('words.txt') == b'alpha beta gamma\n', live.get('words.txt')
assert live.get('result.txt') == b'alpha beta gamma\n', live.get('result.txt')
assert 'nope.txt' not in live, "bad-substitution line created its target"
assert '' not in live, "empty-target line created a file"
print("[test] host: expanded target written, bad line created nothing")
EOF

echo "[test] PASS: expansion in words, stages, targets and assignment RHS"
