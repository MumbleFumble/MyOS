#!/usr/bin/env bash
# Headless test for Phase 9 increment 3: HTTP/1.0 GET under QEMU
# user networking (slirp). A one-shot HTTP server on the host
# listens on port 80 (the guest reaches it as 10.0.2.2). The guest
# takes a DHCP lease, then fetches /hello with `httpget`; the
# serial must show the 200 status line and the fixed body, and the
# server must log the guest's request line.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_http_serial.txt
SOCK=/tmp/qemu_http_mon.sock

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

HTTPLOG=/tmp/myos_http_server.log
rm -f "$SERIAL" "$SOCK" "$HTTPLOG"
python3 "$ROOT/scripts/http_server.py" > "$HTTPLOG" 2>&1 &
HSRV=$!
sleep 0.5
qemu-system-x86_64 -m 256 -display none -vga none \
  -serial "file:$SERIAL" -monitor "unix:$SOCK,server,nowait" \
  -boot d -cdrom "$ISO" -drive "file=$DISK,format=raw,if=ide,index=0" \
  -netdev "user,id=n0" \
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
      '/') k="slash" ;;
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

fail() { echo "[test] FAIL: $1"; kill -9 "$QPID" "$HSRV" 2>/dev/null; exit 1; }

for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || fail "monitor socket never appeared"
sleep 20

grep -a -q "e1000 network up" "$SERIAL" || fail "e1000 did not come up"

printf '%s\n' "dhcp" | send_cmds
wait_for_count "dhcp: bound 10.0.2." 1 60 || fail "no DHCP lease from slirp"

printf '%s\n' "httpget 10.0.2.2 /hello" | send_cmds
wait_for_count "HTTP/1.0 200 OK" 1 60 || fail "no HTTP 200 in guest output"
wait_for_count "MYOS-HTTP-OK phase nine!" 1 30 || fail "HTTP body missing in guest output"

sleep 1
grep -a -q "REQUEST: GET /hello HTTP/1.0" "$HTTPLOG" || fail "server did not log the guest request: $(cat "$HTTPLOG")"

echo "[test] PASS: HTTP/1.0 GET under slirp (DHCP + TCP + HTTP end to end)"
kill -9 "$QPID" "$HSRV" 2>/dev/null
exit 0
