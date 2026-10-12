#!/usr/bin/env bash
# Headless test for the Phase 2 page-fault handler (increment 1: observe).
# Boots MyOS in QEMU (no display), types `pf` in the shell (a deliberate
# ring-3 write to unmapped 0x50000000000), and checks the serial log for
# the handler's [pf] line with the expected CR2 and decoded error code.
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_pf_serial.txt
SOCK=/tmp/qemu_pf_mon.sock

rm -f "$SERIAL" "$SOCK"

qemu-system-x86_64 \
  -cdrom "$ISO" \
  -drive file="$DISK",format=raw,if=ide,index=0 \
  -boot d -m 512M \
  -serial file:"$SERIAL" \
  -monitor unix:"$SOCK",server,nowait \
  -display none &
QPID=$!
echo "[test] QEMU pid=$QPID, booting..."
sleep 6
echo "[test] boot complete"

type_word() {
  local str="$1"
  local i c
  for (( i=0; i<${#str}; i++ )); do
    c="${str:$i:1}"
    case "$c" in
      ' ') echo "sendkey spc" ;;
      '.') echo "sendkey dot" ;;
      '_') echo "sendkey shift-minus" ;;
      *)   echo "sendkey $c" ;;
    esac
    sleep 0.06
  done
  echo "sendkey ret"
  sleep 0.6
}

# Wait until $1 appears in the serial log (or $2 seconds elapse).
wait_for() {
  local needle="$1" timeout="$2" waited=0
  while ! grep -q "$needle" "$SERIAL" 2>/dev/null; do
    sleep 2
    waited=$((waited + 2))
    if [ "$waited" -ge "$timeout" ]; then
      echo "[test] TIMEOUT waiting for '$needle'"
      return 1
    fi
  done
  echo "[test] saw '$needle' after ~${waited}s"
  return 0
}

{
  type_word "pf"
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

PASS=1
if ! wait_for "\[pf\] PAGE FAULT" 60; then
  echo "[test] FAIL: handler never fired"
  PASS=0
fi

# Expected: write to unmapped 0x50000000000 from ring 3 ->
#   cr2 = 0x0000050000000000, err = 0x6 (not-present, write, user-mode)
if ! grep -q "cr2=0x0000050000000000" "$SERIAL"; then
  echo "[test] FAIL: CR2 mismatch"
  PASS=0
fi
if ! grep -q "err=0x0000000000000006" "$SERIAL"; then
  echo "[test] FAIL: error-code mismatch (want 0x6 = not-present/write/user)"
  PASS=0
fi
if ! grep -q "(not-present write user)" "$SERIAL"; then
  echo "[test] FAIL: decoded error-code text mismatch"
  PASS=0
fi

{
  echo "quit"
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

wait "$QPID" 2>/dev/null || true

echo ""
echo "=== [pf] LINES ==="
grep -a "\[pf\]" "$SERIAL" | head -10
echo ""
if [ "$PASS" -eq 1 ]; then
  echo "[test] PASS: page-fault handler fired with expected CR2/err"
else
  echo "[test] FAIL"
  echo "=== FULL SERIAL TAIL ==="
  tail -c 3000 "$SERIAL"
fi
exit $((1 - PASS))
