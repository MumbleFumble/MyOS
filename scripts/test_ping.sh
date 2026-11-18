#!/usr/bin/env bash
# Headless test for Phase 7 increment 2: ARP + IPv4 + ICMP echo.
# scripts/ping_peer.py plays the network over the socket backend:
# it ARPs for the guest (192.168.77.2) and pings it; the guest must
# answer both, with valid checksums on the echo reply.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_ping_serial.txt
SOCK=/tmp/qemu_ping_mon.sock
RESULT=/tmp/ping_peer_result.txt

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

rm -f "$SERIAL" "$SOCK" "$RESULT"
python3 "$ROOT/scripts/ping_peer.py" "$RESULT" &
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

grep -a -q "arp reply to 192.168.77.1" "$SERIAL" || fail "guest serial missing arp reply line"
grep -a -q "icmp echo reply to 192.168.77.1" "$SERIAL" || fail "guest serial missing icmp reply line"

echo "[test] PASS: ARP reply + ICMP echo reply (checksums verified)"
kill -9 "$QPID" "$PEERPID" 2>/dev/null
exit 0
