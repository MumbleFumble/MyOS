#!/usr/bin/env bash
# Headless test for Phase 12 inc.5: tiny httpd serving a MyFS file.
# The test writes hello.txt via the shell, then runs the httpd
# twice: once for the existing file (peer asserts the exact 200
# response, byte for byte) and once for a missing file (peer
# asserts the exact 404 response). The second listen succeeding
# on the same port proves the slot/listener cleanup.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_httpd_serial.txt
SOCK=/tmp/qemu_httpd_mon.sock
RESULT=/tmp/http_peer_result.txt

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1
rm -f "$SERIAL" "$SOCK" "$RESULT"

python3 "$ROOT/scripts/http_peer.py" "$RESULT" &
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
      '-') echo "sendkey minus" ;;
      *)   echo "sendkey $c" ;;
    esac
    sleep 0.06
  done
  echo "sendkey ret"
  sleep 0.6
}
type_cmd() { { type_word "$1"; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true; }
count() { grep -a -c "$1" "$SERIAL" 2>/dev/null || true; }
wait_count() {
  local i
  for i in $(seq 1 "$3"); do
    [ "$(count "$1")" -ge "$2" ] && return 0
    grep -q "PEER FAIL" "$RESULT" 2>/dev/null && return 1
    sleep 1
  done
  return 1
}

type_cmd "write hello.txt hello-myfs"
wait_count "written" 1 30 || fail "fixture file was not written"
type_cmd "httpd 8080 hello.txt"
wait_count "httpd: closed" 1 120 || fail "first httpd run did not close"
type_cmd "httpd 8080 nosuch.txt"
wait_count "httpd: closed" 2 120 || fail "second httpd run (404) did not close"

for i in $(seq 1 60); do
  grep -q "PEER OK" "$RESULT" 2>/dev/null && break
  grep -q "PEER FAIL" "$RESULT" 2>/dev/null && fail "peer verdict: $(cat "$RESULT")"
  sleep 1
done
grep -q "PEER OK" "$RESULT" 2>/dev/null || fail "peer never finished: $(cat "$RESULT" 2>/dev/null || echo none)"
grep -a -q "httpd: accepted 192.168.77.1:40001" "$SERIAL" || fail "guest did not accept the 200 connection"
grep -a -q "httpd: accepted 192.168.77.1:40002" "$SERIAL" || fail "guest did not accept the 404 connection"

echo "[test] PASS: httpd served the MyFS file byte-identical (HTTP/1.0 200), 404 for the missing file, listener freed"
kill -9 "$QPID" "$PEERPID" 2>/dev/null
exit 0
