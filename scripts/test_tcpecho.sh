#!/usr/bin/env bash
# Headless test for Phase 9 increment 2: TCP client.
# QEMU socket netdev (TCP stream mode). The Python peer runs a
# minimal TCP server: it verifies the guest's checksums, SYN, data
# payload and close handshake, sends a greeting, echoes the data
# and closes. The shell must print greeting + echo.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_tcpecho_serial.txt
SOCK=/tmp/qemu_tcpecho_mon.sock
PEERLOG=/tmp/myos_tcpecho_peer.log

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

rm -f "$SERIAL" "$SOCK" "$PEERLOG"
python3 "$ROOT/scripts/tcp_peer.py" > "$PEERLOG" 2>&1 &
PPEER=$!
sleep 0.5

qemu-system-x86_64 -m 256 -display none -vga none \
  -serial "file:$SERIAL" -monitor "unix:$SOCK,server,nowait" \
  -boot d -cdrom "$ISO" -drive "file=$DISK,format=raw,if=ide,index=0" \
  -netdev "socket,id=n0,connect=127.0.0.1:10000" \
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

wait_for_count() {
  local pat="$1" want="$2" timeout="$3" t=0 n=0
  while [ "$t" -lt "$timeout" ]; do
    n=$(grep -a -c "$pat" "$SERIAL" 2>/dev/null || true)
    [ "${n:-0}" -ge "$want" ] && return 0
    sleep 1; t=$((t+1))
  done
  echo "[test] TIMEOUT waiting for $want x '$pat' (saw ${n:-0})"
  return 1
}

fail() { echo "[test] FAIL: $1"; kill -9 "$QPID" "$PPEER" 2>/dev/null; exit 1; }

for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || fail "monitor socket never appeared"
sleep 20

grep -a -q "e1000 network up" "$SERIAL" || fail "e1000 did not come up"

printf '%s\n' "tcpecho 192.168.77.1 8080 hello-myos" | send_cmds
wait_for_count "HELLO-TCP hello-myos" 1 60 || fail "guest did not print greeting + echo"

sleep 1
grep -a -q "PEER OK" "$PEERLOG" || fail "peer did not validate the exchange: $(cat "$PEERLOG")"

echo "[test] PASS: TCP connect/send/recv/close (checksums + seq verified by peer)"
kill -9 "$QPID" "$PPEER" 2>/dev/null
exit 0
