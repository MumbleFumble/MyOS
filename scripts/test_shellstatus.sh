#!/usr/bin/env bash
# Phase 16 inc.3: $? expands to the status (0/1) of the last
# executed line, under the line-level model: every execute_line
# / run_pipeline exit assigns the status except a blank line
# (nothing executed, status unchanged). The dispatch outcome
# is captured by a reset at dispatch entry plus the echo
# fallback raising it; builtin-reported failures (cat: not
# found) therefore count as 0, by design. Asserted: initial 0;
# 0 after a successful command and assignment; 1 after an
# unknown command, a bad substitution, a refused pipeline
# stage; a blank line leaving 1 standing; $? in arg (both
# digits), redirect-target (both digits), assignment-RHS,
# pipeline-stage and script-line positions; $?x concatenation;
# ${?} as bad substitution with no dispatch; a bare $? line
# (the digit is an unknown command); a pipeline keeping 1 if
# ANY stage hit the fallback; a script's first line reading
# the run line's reset (0), a middle failure readable by the
# next script line, and the run line leaving the LAST script
# line's status (0 and 1 variants); an expanded `run` refused
# under redirection by its expanded name, creating nothing.
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
  'statscript.txt': "frobnicate first$?\ncat ${BAD\nIN=$?\ncat words.txt\n",
  'statscript2.txt': "cat words.txt\ncat ${BAD\n",
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
print("[test] injected statscript.txt, statscript2.txt")
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

type_word "frobnicate i\$?"
type_word "write words.txt alpha beta gamma"
type_word "frobnicate a\$?"
type_word "GOOD=ok"
type_word "frobnicate b\$?"
type_word "cat words.txt"
type_word "nosuchcmd"
type_word "frobnicate c\$?"
type_word ""
type_word "frobnicate d\$?"
type_word "cat \${BAD"
type_word "frobnicate e\$?"
type_word "cat \${?}"
type_word "S=\$?"
type_word "S2=\$?"
type_word "cat \$?"
type_word "nosuchcmd"
type_word "cat \$?"
type_word "nosuchcmd"
type_word "frobnicate x\$?y"
type_word "\$?"
type_word "XE=exit"
type_word "cat words.txt | \$XE"
type_word "frobnicate f\$?"
type_word "cat words.txt | cat"
type_word "frobnicate g\$?"
type_word "nosuchcmd | cat"
type_word "frobnicate h\$?"
type_word "cat words.txt > out\$?.txt"
type_word "cat words.txt"
type_word "cat words.txt > out\$?.txt"
type_word "frobnicate p\$? | cat"
type_word "frobnicate q\$?"
type_word "run statscript.txt"
type_word "frobnicate post\$?"
type_word "run statscript2.txt"
type_word "frobnicate post\$?"
type_word "set"
type_word "R=run"
type_word "\$R statscript.txt > rout.txt"

wait_for_count "shell: redirection not supported for run" 1 || fail "expanded-run refusal missing"
sleep 3

check "echo: frobnicate i0" 1
check "echo: frobnicate a0" 1
check "echo: frobnicate b0" 1
check "echo: frobnicate c1" 1
check "echo: frobnicate d1" 1
check "echo: frobnicate e1" 1
check "echo: frobnicate f1" 1
check "echo: frobnicate g0" 1
check "echo: frobnicate h1" 1
check "echo: frobnicate x1y" 1
check "echo: frobnicate p0" 1
check "echo: frobnicate q1" 1
check "echo: frobnicate first0" 1
check "echo: frobnicate post0" 1
check "echo: frobnicate post1" 1
check "echo: nosuchcmd" 4
check "echo: 1" 1
check "shell: bad substitution" 4
check "cat: not found: 0" 1
check "cat: not found: 1" 1
check "S=1" 1
check "S2=0" 1
check "IN=1" 1
check "shell: exit not supported in pipelines" 1
check "shell: redirection not supported for run" 1
check "alpha beta gamma" 5
check "> " 41

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
assert live.get('out1.txt') == b'alpha beta gamma\n', live.get('out1.txt')
assert live.get('out0.txt') == b'alpha beta gamma\n', live.get('out0.txt')
assert 'rout.txt' not in live, "refused expanded run created its target"
print("[test] host: $?-named targets written, refused run created nothing")
EOF

echo "[test] PASS: line-level \$? status in every expansion position"
