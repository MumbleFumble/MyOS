#!/usr/bin/env bash
# Headless test for Phase 7 increment 3 (ping part): the guest shell
# runs `ping 192.168.77.1`; scripts/pingcmd_peer.py answers the ARP
# request and all four echo requests. The shell must print a
# "reply from 192.168.77.1" line for each.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_pingcmd_serial.txt
SOCK=/tmp/qemu_pingcmd_mon.sock
RESULT=/tmp/pingcmd_peer_result.txt

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

rm -f "$SERIAL" "$SOCK" "$RESULT"
python3 "$ROOT/scripts/pingcmd_peer.py" "$RESULT" &
PEERPID=$!
qemu-system-x86_64 -m 256 -display none -vga none \
  -serial "file:$SERIAL" -monitor "unix:$SOCK,server,nowait" \
  -boot d -cdrom "$ISO" -drive "file=$DISK,format=raw,if=ide,index=0" \
  -netdev "socket,id=n0,listen=127.0.0.1:10000" \
  -device "e1000,netdev=n0,mac=52:54:00:12:34:56" &
QPID=$!

type_word() {
  local w="$1"
  for ((i=0; i<${#w}; i++)); do
    local c="${w:i:1}"
    case "$c" in
      [a-z]) k="$c" ;;
      [0-9]) k="$c" ;;
      ' ') k="spc" ;;
      '.') k="dot" ;;
      '-') k="minus" ;;
      '_') k="shift-minus" ;;
      *) echo "[test] no key mapping for '$c'" >&2; return 1 ;;
    esac
    printf 'sendkey %s\n' "$k" | socat - "UNIX-CONNECT:$SOCK" >/dev/null
    sleep 0.06
  done
}

send_cmds() {
  local line
  while IFS= read -r line; do
    type_word "$line"
    printf 'sendkey ret\n' | socat - "UNIX-CONNECT:$SOCK" >/dev/null
    sleep 0.3
  done
}

fail() { echo "[test] FAIL: $1"; kill -9 "$QPID" "$PEERPID" 2>/dev/null; exit 1; }

for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || fail "monitor socket never appeared"
sleep 20

printf '%s\n' "ping 192.168.77.1" | send_cmds

ok=0
for i in $(seq 1 60); do
  n=$(grep -a -c "reply from 192.168.77.1" "$SERIAL" 2>/dev/null || true)
  [ "${n:-0}" -ge 4 ] && { ok=1; break; }
  sleep 1
done
[ "$ok" -eq 1 ] || fail "shell printed only ${n:-0} ping replies"

for i in $(seq 1 30); do [ -f "$RESULT" ] && break; sleep 1; done
grep -q "PEER OK" "$RESULT" 2>/dev/null || fail "peer verdict: $(cat "$RESULT" 2>/dev/null || echo none)"

echo "[test] PASS: shell ping got 4 replies under cross-traffic (TX mutex contention)"
kill -9 "$QPID" "$PEERPID" 2>/dev/null
exit 0
