#!/usr/bin/env bash
# Headless test for Phase 14 inc.5: execute redirection.
#
# Drives the shell on a COPY of the disk image and asserts:
#  - "cat words.txt > out.txt" runs silently on the console
#    and out.txt is byte-identical to words.txt (host check);
#    redirecting again truncates (still one copy);
#  - the shell prompt stays on the console (the command after
#    a redirected one prints normally, so fd 1 was restored);
#  - "ls > listing.txt" captures the listing (which includes
#    listing.txt itself: the target is created before the
#    command runs) and prints nothing to the console;
#  - "write note.txt hello world > wlog.txt" stores the text
#    in note.txt and the builtin's own output ("written") in
#    wlog.txt;
#  - a pipeline line with a target EXECUTES (inc.6) as a
#    pass-through (inc.7): the last stage is a bare cat, so
#    the data itself lands in pipe.txt
#    message and creates nothing;
#  - an unopenable target (a 70-char name; MyFS names are
#    capped at 63) prints "shell: cannot create", runs
#    nothing, and fd 1 still works afterwards.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
TMP="$(mktemp -d /tmp/myos_redir.XXXXXX)"

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
LONGNAME=$(printf 'a%.0s' $(seq 1 70))
{
  type_word "write words.txt alpha beta gamma"
  type_word "cat words.txt > out.txt"
  type_word "cat out.txt"
  type_word "cat words.txt > out.txt"
  type_word "cat words.txt | cat > pipe.txt"
  type_word "ls > listing.txt"
  type_word "cat listing.txt"
  type_word "write note.txt hello world > wlog.txt"
  type_word "cat words.txt > $LONGNAME"
  type_word "cat words.txt"
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

count() { grep -a -c "$1" "$2" 2>/dev/null || true; }
for i in $(seq 1 60); do
  [ "$(count 'alpha beta gamma' "$SER")" -ge 2 ] && break
  sleep 1
done
sleep 2

[ "$(count 'alpha beta gamma' "$SER")" -eq 2 ] || fail "content count != 2 (redirected cats leaked to console, or post-error cat failed)"
[ "$(count 'written' "$SER")" -eq 1 ] || fail "console 'written' count != 1 (redirected write output leaked, or plain write broke)"
[ "$(count 'hello world' "$SER")" -eq 0 ] || fail "'hello world' reached the console"
[ "$(count 'pipelines not yet supported' "$SER")" -eq 0 ] || fail "pipelines message should be gone"
[ "$(count 'usage: cat' "$SER")" -eq 0 ] || fail "usage printed although the piped bare cat should copy into pipe.txt"
[ "$(count 'shell: cannot create' "$SER")" -eq 1 ] || fail "cannot-create error missing"
[ "$(count 'listing.txt' "$SER")" -eq 1 ] || fail "listing.txt not shown exactly once (by its cat)"

python3 - "$IMG" << 'PYEOF' || fail "host-side image check failed"
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
assert live.get('words.txt') == b'alpha beta gamma\n', live.get('words.txt')
assert live.get('out.txt') == b'alpha beta gamma\n', live.get('out.txt')
assert live.get('note.txt') == b'hello world\n', live.get('note.txt')
assert live.get('wlog.txt') == b'written\r\n', live.get('wlog.txt')
lst = live.get('listing.txt', b'')
assert b'words.txt' in lst and b'out.txt' in lst and b'listing.txt' in lst, lst
assert b'wlog.txt' not in lst and b'note.txt' not in lst, lst
assert live.get('pipe.txt') == b'alpha beta gamma\n', live.get('pipe.txt')
assert not any(len(k) >= 64 for k in live), list(live)
print("[test] host-side image check: out.txt byte-identical (truncated on re-redirect), wlog/note split correct, listing captured, pipe.txt holds the passed-through data, no long-name file")
PYEOF

echo "[test] PASS: redirection executes with exact restore, truncation, and honest exec/open-failure behaviour"
cleanup
exit 0
