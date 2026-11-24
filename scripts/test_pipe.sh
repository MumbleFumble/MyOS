#!/usr/bin/env bash
# Headless test for Phase 14 inc.3: pipes.
#
# Boots a COPY of the default disk image and runs pipetest.elf,
# which asserts the declared non-blocking pipe semantics in
# the guest: FIFO order across interleaved chunks, a 4000-byte
# wrap-around run through the 1024-byte ring, fill/full -2 /
# partial-space / drain behaviour, drain-then-EOF after the
# write end closes, write -1 after the read end closes, dup2
# aliases of either end behaving as the same end, wrong-end
# operations failing, and pipe() failing cleanly with fewer
# than two free slots. PASS requires "pipetest: ALL PASS"
# with zero "pipetest: FAIL" lines.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
TMP="$(mktemp -d /tmp/myos_pipe.XXXXXX)"

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
      *)   echo "sendkey $c" ;;
    esac
    sleep 0.06
  done
  echo "sendkey ret"
  sleep 0.6
}
{ type_word "exec pipetest.elf"; sleep 1; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

count() { grep -a -c "$1" "$2" 2>/dev/null || true; }
for i in $(seq 1 90); do
  [ "$(count 'pipetest: fails=' "$SER")" -ge 1 ] && break
  sleep 1
done
[ "$(count 'pipetest: fails=' "$SER")" -ge 1 ] || fail "pipetest did not run to completion"
[ "$(count 'pipetest: FAIL' "$SER")" -eq 0 ] || fail "pipetest reported failures: $(grep -a 'pipetest: FAIL' "$SER" | head -3)"
[ "$(count 'pipetest: ALL PASS' "$SER")" -ge 1 ] || fail "pipetest: ALL PASS missing"
OKS=$(count 'pipetest: ok' "$SER")
[ "$OKS" -ge 45 ] || fail "only $OKS pipetest checks printed"
grep -a "pipetest: ok wrap-around: 4000 bytes in exact order" "$SER" >/dev/null || fail "wrap-around check missing"
grep -a "pipetest: ok reader drains buffered bytes" "$SER" >/dev/null || fail "drain check missing"
grep -a "pipetest: ok write via alias still works (pipe not ended)" "$SER" >/dev/null || fail "alias check missing"

echo "[test] pipetest checks passed: $OKS (FIFO interleave, wrap-around, full/-2/drain, EOF semantics, end aliases, slot exhaustion)"
echo "[test] PASS: pipes deliver one FIFO byte stream with the declared non-blocking empty/full semantics"
cleanup
exit 0
