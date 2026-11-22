#!/usr/bin/env bash
# Headless regression test for Phase 12 inc.4 (review-001):
# server slot cleanup. Run 1 of `tcplisten 8080` is flooded with
# half-open SYNs and times out; the fixed cleanup must free the
# listener and the SYN_RCVD slots so run 2 accepts. Run 3's
# connection is killed by an RST; run 4 must still accept.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_tcprst_serial.txt
SOCK=/tmp/qemu_tcprst_mon.sock
RESULT=/tmp/tcp_rst_peer_result.txt

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1
rm -f "$SERIAL" "$SOCK" "$RESULT"

python3 "$ROOT/scripts/tcp_rst_peer.py" "$RESULT" &
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
run_listen() { { type_word "tcplisten 8080"; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true; }
count() { grep -a -c "$1" "$SERIAL" 2>/dev/null || true; }
wait_count() { # pattern, wanted, budget_secs
  local i
  for i in $(seq 1 "$3"); do
    [ "$(count "$1")" -ge "$2" ] && return 0
    grep -q "PEER FAIL" "$RESULT" 2>/dev/null && return 1
    sleep 1
  done
  return 1
}

run_listen                                    # run 1: floods, times out
wait_count "tcplisten: accept timed out" 1 120 || fail "run 1 did not time out"
run_listen                                    # run 2: must accept (phase A)
wait_count "tcplisten: closed" 1 120 || fail "run 2 did not accept+close after the timeout cleanup"
run_listen                                    # run 3: accepted, then RST (phase B)
wait_count "tcplisten: closed" 2 120 || fail "run 3 did not close after the RST"
run_listen                                    # run 4: must accept again (phase C)

for i in $(seq 1 120); do
  grep -q "PEER OK" "$RESULT" 2>/dev/null && break
  grep -q "PEER FAIL" "$RESULT" 2>/dev/null && fail "peer verdict: $(cat "$RESULT")"
  sleep 1
done
grep -q "PEER OK" "$RESULT" 2>/dev/null || fail "peer never finished: $(cat "$RESULT" 2>/dev/null || echo none)"
[ "$(count 'tcplisten: accepted 192.168.77.1:40001')" -ge 2 ] || fail "expected two accepted connections from :40001"
[ "$(count 'tcplisten: accepted 192.168.77.1:40002')" -ge 1 ] || fail "expected the RST-killed connection from :40002 to be accepted first"
[ "$(count 'tcplisten: closed')" -ge 3 ] || fail "expected three completed closes"

echo "[test] PASS: accept-timeout and RST paths free their slots; re-listen succeeds"
kill -9 "$QPID" "$PEERPID" 2>/dev/null
exit 0
