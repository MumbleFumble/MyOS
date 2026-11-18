#!/usr/bin/env bash
# Headless test for Phase 7 increment 3 (UDP part): the guest runs a
# UDP echo server on port 7. scripts/udp_peer.py sends a datagram
# from 192.168.77.1:5000 and validates the echoed reply.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_udp_serial.txt
SOCK=/tmp/qemu_udp_mon.sock
RESULT=/tmp/udp_peer_result.txt

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

rm -f "$SERIAL" "$SOCK" "$RESULT"
python3 "$ROOT/scripts/udp_peer.py" "$RESULT" &
PEERPID=$!
qemu-system-x86_64 -m 256 -display none -vga none \
  -serial "file:$SERIAL" -monitor "unix:$SOCK,server,nowait" \
  -boot d -cdrom "$ISO" -drive "file=$DISK,format=raw,if=ide,index=0" \
  -netdev "socket,id=n0,listen=127.0.0.1:10000" \
  -device "e1000,netdev=n0,mac=52:54:00:12:34:56" &
QPID=$!

fail() { echo "[test] FAIL: $1"; kill -9 "$QPID" "$PEERPID" 2>/dev/null; exit 1; }

for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || fail "monitor socket never appeared"

for i in $(seq 1 120); do
  grep -q "PEER OK" "$RESULT" 2>/dev/null && break
  grep -q "PEER FAIL" "$RESULT" 2>/dev/null && fail "peer verdict: $(cat "$RESULT")"
  sleep 1
done
grep -q "PEER OK" "$RESULT" 2>/dev/null || fail "peer never finished: $(cat "$RESULT" 2>/dev/null || echo none)"
grep -a -q "udp echo reply to 192.168.77.1" "$SERIAL" || fail "guest serial missing udp echo line"

echo "[test] PASS: UDP echo on port 7"
kill -9 "$QPID" "$PEERPID" 2>/dev/null
exit 0
