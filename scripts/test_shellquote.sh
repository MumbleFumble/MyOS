#!/usr/bin/env bash
# Phase 16 inc.4: quoting. The parser's scanner and
# expand_str share one quote state machine (qstep): token
# slices stay raw, cooking happens in expand_str. Asserted:
# double/single-quoted phrases as one argument (via cat,
# which takes its arg whole); $NAME/$? literal inside single
# quotes, expanded inside double quotes; quoted | and >
# are literal (no pipeline, no file); adjacent concatenation
# a"b c"d; mixed quotes ("it's", 'say "hi"'); an empty
# quoted word staying in arg position (the double space);
# "" and '' alone as the empty-command error, and as a
# pipeline stage; quoted assignment values with spaces
# stored whole (single-quoted $ stays literal); a quoted
# name is not an assignment ("X=1", X"=1", 'X'=1 all fall to
# dispatch); quoted redirect targets with spaces create
# exactly those files; ${"W"} is a bad substitution (the
# brace scan never consults quote state); $"x" cooks to $x;
# a lone trailing $ survives the fallback's whole-line
# expansion; backslash is ordinary; an unterminated quote
# is a parse error (status 1 after it, and the tokeniser's
# error wins over the grammar's on | "abc); script lines
# behave identically, including quoted session commands
# refused by their cooked names and two unterminated lines
# the script continues past.
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
  'quotescript.txt': 'cat "sp ace.txt"\n"exit"\nE=exit\n$E\ne"xi"t\ncat "unterminated\ncat \'also unterminated\nfrobnicate end$?\n',
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
print("[test] injected quotescript.txt")
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
      '\') k="backslash" ;;
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

type_word "write words.txt alpha beta gamma"
type_word "cat words.txt > \"sp ace.txt\""
type_word "cat \"sp ace.txt\""
type_word "cat 'sp ace.txt'"
type_word "W=world"
type_word "frobnicate '\$W'"
type_word "frobnicate \"\$W\""
type_word "frobnicate '\$?'"
type_word "frobnicate \"\$?\""
type_word "cat words.txt"
type_word "frobnicate \"s\$?\""
type_word "frobnicate \"a|b\""
type_word "frobnicate 'a>b'"
type_word "frobnicate a\"b c\"d"
type_word "frobnicate \"it's\""
type_word "frobnicate 'say \"hi\"'"
type_word "frobnicate z\"\"z"
type_word "frobnicate x\"\$\""
type_word "frobnicate \$\"x\""
type_word "frobnicate '\${W}'"
type_word "cat \${\"W\"}"
type_word "cat pre \"\" post"
type_word "\"\""
type_word "''"
type_word "cat words.txt | \"\""
type_word "Q=\"a b c\""
type_word "Q2='x \$W y'"
type_word "Q3=\"p\$W q\""
type_word "\"X=1\""
type_word "X\"=1\""
type_word "'X'=1"
type_word "cat \"abc"
type_word "frobnicate s\$?"
type_word "| \"abc"
type_word "frobnicate 'a\\b'"
type_word "frobnicate z\$"
type_word "cat words.txt > 'tgt file.txt'"
type_word "run quotescript.txt"
type_word "set"

wait_for_count "Q3=pworld q" 1 || fail "quoted assignment values missing"
sleep 3

check "alpha beta gamma" 4
check "echo: frobnicate \$W" 1
check "echo: frobnicate world" 1
check "echo: frobnicate \$?" 1
check "echo: frobnicate 1" 1
check "echo: frobnicate s0" 1
check "echo: frobnicate a|b" 1
check "echo: frobnicate a>b" 1
check "echo: frobnicate ab cd" 1
check "echo: frobnicate it's" 1
check 'echo: frobnicate say "hi"' 1
check "echo: frobnicate zz" 1
check "echo: frobnicate x\$" 1
check "echo: frobnicate \$x" 1
check "echo: frobnicate \${W}" 1
check "shell: bad substitution" 1
check "cat: not found: pre  post" 1
check "shell: empty command" 3
check "echo: X=1" 3
check "shell: parse error: unterminated quote" 4
check "empty command in pipeline" 0
check "echo: frobnicate s1" 1
check "echo: frobnicate a\\b" 1
check "echo: frobnicate z\$" 1
check "run: exit not allowed in a script" 3
check "echo: frobnicate end1" 1
check "Q=a b c" 1
check "Q2=x \$W y" 1
check "Q3=pworld q" 1
check "> " 40

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
assert live.get('sp ace.txt') == b'alpha beta gamma\n', live.get('sp ace.txt')
assert live.get('tgt file.txt') == b'alpha beta gamma\n', live.get('tgt file.txt')
assert 'b' not in live, "quoted > created a file"
print("[test] host: quoted targets created exactly, quoted > created nothing")
EOF

echo "[test] PASS: quoting in words, stages, targets, RHS and scripts"
