#!/usr/bin/env bash
# Headless test for Phase 14 inc.2: dup2 shared descriptions.
#
# Boots a COPY of the default disk image and runs duptest.elf,
# which asserts the shared-offset / refcount semantics in the
# guest: interleaved reads observe one offset (012 then 345),
# interleaved writes form one stream (abcd), closing one
# alias leaves the other usable, the last close kills both
# numbers with the file intact, displacement, invalid args,
# the save/displace/restore redirection shape over fd 1
# (bytes written via fd 1 land in the file), console aliasing
# (a marker line written via the alias fd), and the 13-fd
# table limit. PASS requires "duptest: ALL PASS", zero
# "duptest: FAIL" lines, and the console-alias marker.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
TMP="$(mktemp -d /tmp/myos_dup2.XXXXXX)"

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
{ type_word "exec duptest.elf"; sleep 1; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

count() { grep -a -c "$1" "$2" 2>/dev/null || true; }
for i in $(seq 1 90); do
  [ "$(count 'duptest: fails=' "$SER")" -ge 1 ] && break
  sleep 1
done
[ "$(count 'duptest: fails=' "$SER")" -ge 1 ] || fail "duptest did not run to completion"
[ "$(count 'duptest: FAIL' "$SER")" -eq 0 ] || fail "duptest reported failures: $(grep -a 'duptest: FAIL' "$SER" | head -3)"
[ "$(count 'duptest: ALL PASS' "$SER")" -ge 1 ] || fail "duptest: ALL PASS missing"
OKS=$(count 'duptest: ok' "$SER")
[ "$OKS" -ge 50 ] || fail "only $OKS duptest checks printed (expected 52)"
grep -a "duptest: ok read via alias continues at shared offset (345)" "$SER" >/dev/null || fail "shared-offset check missing"
grep -a "duptest: ok bytes written via fd 1 landed in the file" "$SER" >/dev/null || fail "redirection check missing"
# The console-alias marker was written through fd 6 (a dup of
# fd 1): it must appear in the serial log as guest output.
[ "$(count 'duptest: console via alias' "$SER")" -ge 1 ] || fail "console alias marker missing from serial"

echo "[test] duptest checks passed: $OKS (shared offset reads/writes, alias close semantics, displacement, redirection over fd 1, console alias, table limit)"
echo "[test] PASS: dup2 aliases share one description and offset; backend closes exactly once at the last alias"
cleanup
exit 0
