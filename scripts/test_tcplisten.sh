#!/usr/bin/env bash
# Headless test for Phase 12 inc.2: TCP listen + accept.
# The shell runs `tcplisten 8080`; scripts/tcp_listen_peer.py
# connects from 192.168.77.1:40001, completes the handshake
# (validating the guest's SYN-ACK), and plays its half of the
# close. The guest must print the accepted peer and free the slot
# (its close completes), and the peer must see a clean FIN swap.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_tcplisten_serial.txt
SOCK=/tmp/qemu_tcplisten_mon.sock
RESULT=/tmp/tcp_listen_peer_result.txt

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1
rm -f "$SERIAL" "$SOCK" "$RESULT"

python3 "$ROOT/scripts/tcp_listen_peer.py" "$RESULT" &
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
{ type_word "tcplisten 8080"; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

for i in $(seq 1 170); do
  grep -q "PEER OK" "$RESULT" 2>/dev/null && break
  grep -q "PEER FAIL" "$RESULT" 2>/dev/null && fail "peer verdict: $(cat "$RESULT")"
  sleep 1
done
grep -q "PEER OK" "$RESULT" 2>/dev/null || fail "peer never finished: $(cat "$RESULT" 2>/dev/null || echo none)"

grep -a -q "tcplisten: accepted 192.168.77.1:40001" "$SERIAL" || fail "guest did not report the accepted peer"
grep -a -q "tcplisten: closed" "$SERIAL" || fail "guest did not complete the close (slot not freed)"

echo "[test] PASS: TCP listen/accept handshake, peer reported, slot freed on close"
kill -9 "$QPID" "$PEERPID" 2>/dev/null
exit 0
