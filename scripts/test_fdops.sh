#!/usr/bin/env bash
# Headless test for Phase 14 inc.1: the fd vtable.
#
# Boots a COPY of the default disk image (so fdtest's file
# does not pollute the shared one) and runs the guest program
# fdtest.elf, which in one run exercises both dispatch paths
# through the per-fd ops — file fds (fcreate/write/close/
# reopen/read/verify) and console fds (stdout writes) — and
# asserts the edge semantics the vtable must preserve:
# write(stdin) -1, read(stdout/stderr) -1, close(console) -1,
# closed/free fd -1, double close -1, EOF read 0, and the
# count==0 asymmetries (read count 0 is 0 even on an invalid
# fd; write count 0 is 0 on stdout but -1 on a free fd).
# PASS requires fdtest's own "fdtest: ALL PASS" with zero
# "fdtest: FAIL" lines.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
TMP="$(mktemp -d /tmp/myos_fdops.XXXXXX)"

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
{ type_word "exec fdtest.elf"; sleep 1; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

count() { grep -a -c "$1" "$2" 2>/dev/null || true; }
for i in $(seq 1 90); do
  [ "$(count 'fdtest: fails=' "$SER")" -ge 1 ] && break
  sleep 1
done
[ "$(count 'fdtest: fails=' "$SER")" -ge 1 ] || fail "fdtest did not run to completion"
[ "$(count 'fdtest: FAIL' "$SER")" -eq 0 ] || fail "fdtest reported failures: $(grep -a 'fdtest: FAIL' "$SER" | head -3)"
[ "$(count 'fdtest: ALL PASS' "$SER")" -ge 1 ] || fail "fdtest: ALL PASS missing"
OKS=$(count 'fdtest: ok' "$SER")
[ "$OKS" -ge 19 ] || fail "only $OKS fdtest checks printed (expected 19)"
grep -a "fdtest: ok read-back bytes identical" "$SER" >/dev/null || fail "byte verification check missing"
grep -a "fdtest: ok write to stdin is -1" "$SER" >/dev/null || fail "console edge check missing"

echo "[test] fdtest checks passed: $OKS (file write/close/reopen/read/verify + console edges + count==0 asymmetries)"
echo "[test] PASS: fd dispatch goes through the per-fd ops for file and console fds alike, with the pre-vtable edge semantics preserved"
cleanup
exit 0
