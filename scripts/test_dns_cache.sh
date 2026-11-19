#!/usr/bin/env bash
# Headless test for Phase 11 inc.3: DNS answer caching.
# scripts/dns_cache_peer.py logs every query it receives:
#   lookup example.com      -> 1 query, answer 203.0.113.77 (TTL 12)
#   lookup example.com      -> still 1 query (cache hit)
#   lookup other.com        -> 1 query for other.com
#   wait 13s, lookup again   -> 2 queries (TTL expired, re-fetched)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_dnscache_serial.txt
SOCK=/tmp/qemu_dnscache_mon.sock
LOG=/tmp/dns_cache_queries.log

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1
rm -f "$SERIAL" "$SOCK" "$LOG"

python3 "$ROOT/scripts/dns_cache_peer.py" /dev/null "$LOG" &
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
send_cmds() {
  { while IFS= read -r line; do type_word "$line"; sleep 1; done; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true
}
count() { grep -c "^$1\$" "$LOG" 2>/dev/null || true; }

printf '%s\n' "nslookup example.com 192.168.77.1" | send_cmds
sleep 2
[ "$(count example.com)" = "1" ] || fail "after 1st lookup, example.com queries=$(count example.com)"
grep -a -q "example.com = 203.0.113.77" "$SERIAL" || fail "1st answer wrong/missing"

printf '%s\n' "nslookup example.com 192.168.77.1" | send_cmds
sleep 2
[ "$(count example.com)" = "1" ] || fail "2nd lookup was not a cache hit (queries=$(count example.com))"

printf '%s\n' "nslookup other.com 192.168.77.1" | send_cmds
sleep 2
[ "$(count other.com)" = "1" ] || fail "other.com queries=$(count other.com)"
grep -a -q "other.com = 203.0.113.78" "$SERIAL" || fail "other.com answer wrong/missing"

sleep 13
printf '%s\n' "nslookup example.com 192.168.77.1" | send_cmds
sleep 2
[ "$(count example.com)" = "2" ] || fail "post-TTL lookup did not re-fetch (queries=$(count example.com))"

echo "[test] PASS: DNS cache hits, per-name entries, TTL expiry re-fetch"
kill -9 "$QPID" "$PEERPID" 2>/dev/null
exit 0
