#!/usr/bin/env bash
# Headless test for Phase 11 inc.4: real argv passing for exec.
#   exec argtest.elf one two three -> argc=4, argv[0..3] printed
#   exec argtest.elf               -> argc=1
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_argv_serial.txt
SOCK=/tmp/qemu_argv_mon.sock

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1
rm -f "$SERIAL" "$SOCK"

qemu-system-x86_64 -m 256 -display none -vga none \
  -serial "file:$SERIAL" -monitor "unix:$SOCK,server,nowait" \
  -boot d -cdrom "$ISO" -drive "file=$DISK,format=raw,if=ide,index=0" \
  -netdev user,id=n0 -device e1000,netdev=n0 &
QPID=$!

fail() { echo "[test] FAIL: $1"; kill -9 "$QPID" 2>/dev/null; exit 1; }

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
  { while IFS= read -r line; do type_word "$line"; sleep 2; done; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true
}

printf '%s\n' "exec argtest.elf one two three" "exec argtest.elf" | send_cmds
sleep 3

grep -a -q "argc=4" "$SERIAL" || fail "argc=4 not printed"
grep -a -q "argv\[0\]=argtest.elf" "$SERIAL" || fail "argv[0] wrong"
grep -a -q "argv\[1\]=one" "$SERIAL" || fail "argv[1] wrong"
grep -a -q "argv\[2\]=two" "$SERIAL" || fail "argv[2] wrong"
grep -a -q "argv\[3\]=three" "$SERIAL" || fail "argv[3] wrong"
grep -a -q "argc=1" "$SERIAL" || fail "argc=1 (no args) not printed"

echo "[test] PASS: exec passes real argc/argv to user programs"
kill -9 "$QPID" 2>/dev/null
exit 0
