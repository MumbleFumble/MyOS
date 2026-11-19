#!/usr/bin/env bash
# Headless test for Phase 11 inc.1: incoming checksum verification.
# scripts/cksum_peer.py sends good and corrupted ICMP/UDP/TCP packets;
# the guest must answer the good ones and silently drop the bad ones.
# Afterwards the shell's netstat must show badck >= 4.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_cksum_serial.txt
SOCK=/tmp/qemu_cksum_mon.sock
RESULT=/tmp/cksum_peer_result.txt

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

rm -f "$SERIAL" "$SOCK" "$RESULT"
python3 "$ROOT/scripts/cksum_peer.py" "$RESULT" &
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

for i in $(seq 1 150); do
  grep -q "PEER OK" "$RESULT" 2>/dev/null && break
  grep -q "PEER FAIL" "$RESULT" 2>/dev/null && fail "peer verdict: $(cat "$RESULT")"
  sleep 1
done
grep -q "PEER OK" "$RESULT" 2>/dev/null || fail "peer never finished: $(cat "$RESULT" 2>/dev/null || echo none)"

# Ask the shell for netstat; badck must count the 4 corrupted packets.
type_word() {
  local str="$1" i c
  for (( i=0; i<${#str}; i++ )); do
    c="${str:$i:1}"
    case "$c" in
      ' ') echo "sendkey spc" ;;
      *)   echo "sendkey $c" ;;
    esac
    sleep 0.06
  done
  echo "sendkey ret"
  sleep 0.6
}
{ type_word "netstat"; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true
sleep 3
BADCK=$(grep -a -o "badck=[0-9]*" "$SERIAL" | tail -1 | cut -d= -f2)
echo "[test] netstat badck=${BADCK:-none} (expect >= 4)"
[ -n "$BADCK" ] && [ "$BADCK" -ge 4 ] || fail "badck counter ${BADCK:-missing} < 4"

echo "[test] PASS: bad checksums dropped (IP/ICMP/UDP/TCP), good packets answered"
kill -9 "$QPID" "$PEERPID" 2>/dev/null
exit 0
