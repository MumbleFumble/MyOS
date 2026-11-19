#!/usr/bin/env bash
# Headless test for Phase 11 inc.3: DHCP lease renewal.
# scripts/dhcp_peer.py serves a 20-second lease on the socket
# backend; after the shell binds with `dhcp`, the guest must renew
# by itself at T1 (~10 s). Two renewals observed by the peer, and
# two "dhcp renewed" lines in the guest serial, pass the test.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_dhcprenew_serial.txt
SOCK=/tmp/qemu_dhcprenew_mon.sock
RESULT=/tmp/dhcp_peer_result.txt

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1
rm -f "$SERIAL" "$SOCK" "$RESULT"

python3 "$ROOT/scripts/dhcp_peer.py" "$RESULT" &
PEERPID=$!
qemu-system-x86_64 -m 256 -display none -vga none \
  -serial "file:$SERIAL" -monitor "unix:$SOCK,server,nowait" \
  -boot d -cdrom "$ISO" -drive "file=$DISK,format=raw,if=ide,index=0" \
  -netdev "socket,id=n0,listen=127.0.0.1:10000" \
  -device "e1000,netdev=n0,mac=52:54:00:12:34:56" &
QPID=$!

fail() { echo "[test] FAIL: $1"; kill -9 "$QPID" "$PEERPID" 2>/dev/null; exit 1; }

for i in $(seq 1 40); do [ -S "$SOCK" ] && break; sleep 0.5; done
sleep 20

type_word() {
  local str="$1" i c
  for (( i=0; i<${#str}; i++ )); do
    c="${str:$i:1}"
    case "$c" in
      ' ') echo "sendkey spc" ;;
      '.') echo "sendkey dot" ;;
      *)   echo "sendkey $c" ;;
    esac
    sleep 0.06
  done
  echo "sendkey ret"
  sleep 0.6
}
{ type_word "dhcp"; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

for i in $(seq 1 170); do
  grep -q "PEER OK" "$RESULT" 2>/dev/null && break
  grep -q "PEER FAIL" "$RESULT" 2>/dev/null && fail "peer verdict: $(cat "$RESULT")"
  sleep 1
done
grep -q "PEER OK" "$RESULT" 2>/dev/null || fail "peer never finished: $(cat "$RESULT" 2>/dev/null || echo none)"

grep -a -q "dhcp: bound 192.168.77.2" "$SERIAL" || fail "guest never bound"
N=$(grep -a -c "dhcp renewed" "$SERIAL")
echo "[test] renewals logged by guest: $N"
[ "$N" -ge 2 ] || fail "guest logged only $N renewals"

echo "[test] PASS: DHCP lease renewed autonomously at T1 (lease 20s)"
kill -9 "$QPID" "$PEERPID" 2>/dev/null
exit 0
