#!/usr/bin/env bash
# Headless test for Phase 7 increment 1: e1000 raw frames.
# QEMU runs with a socket (TCP stream) netdev; scripts/net_peer.py plays the
# network: it validates the guest's broadcast hello frame (sent via
# the shell `netsend` command) and replies with a unicast frame whose
# payload the guest's RX task must log to serial.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_net_serial.txt
SOCK=/tmp/qemu_net_mon.sock
RESULT=/tmp/net_peer_result.txt

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

rm -f "$SERIAL" "$SOCK" "$RESULT"
python3 "$ROOT/scripts/net_peer.py" "$RESULT" &
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

fail() { echo "[test] FAIL: $1"; kill -9 "$QPID" "$PEERPID" 2>/dev/null; exit 1; }

for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || fail "monitor socket never appeared"
sleep 20

grep -a -q "e1000 network up" "$SERIAL" || fail "e1000 did not come up"

printf '%s\n' "netstat" | send_cmds
wait_for_count "mac=52:54:00:12:34:56" 1 30 || fail "netstat MAC wrong or missing"
grep -a -q "link=up" "$SERIAL" || fail "link not up"

printf '%s\n' "netsend" | send_cmds
wait_for_count "netsent" 1 30 || fail "netsend failed in guest"

# Peer validates the hello and sends its frame; the guest RX task
# must log the payload to serial.
wait_for_count "HOST-TO-MYOS-42" 1 60 || fail "guest never logged the peer frame"

for i in $(seq 1 30); do [ -f "$RESULT" ] && break; sleep 1; done
grep -q "PEER OK" "$RESULT" 2>/dev/null || fail "peer verdict: $(cat "$RESULT" 2>/dev/null || echo none)"

echo "[test] PASS: e1000 raw TX+RX over socket backend"
kill -9 "$QPID" "$PEERPID" 2>/dev/null
exit 0
