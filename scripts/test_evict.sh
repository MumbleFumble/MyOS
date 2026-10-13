#!/usr/bin/env bash
# Headless test for Phase 2 eviction (increment 4).
# Boots MyOS, runs `exec evict_test.elf` (16 pages, 4-frame FIFO quota),
# and checks:
#   1. "[evict] PASS" appears (test completed without halt)
#   2. exactly 17 "[pf] demand-mapped" lines (16 pages + page 0 re-touch)
#   3. exactly 13 "[pf] evicting" lines (12 for pages 4..15, 1 for re-touch)
#   4. no "halting" line appears
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_evict_serial.txt
SOCK=/tmp/qemu_evict_mon.sock

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1
rm -f "$SERIAL" "$SOCK"

qemu-system-x86_64 \
  -cdrom "$ISO" \
  -drive file="$DISK",format=raw,if=ide,index=0 \
  -boot d -m 512M \
  -serial file:"$SERIAL" \
  -monitor unix:"$SOCK",server,nowait \
  -display none &
QPID=$!

for i in $(seq 1 40); do [ -S "$SOCK" ] && break; sleep 0.5; done
sleep 20

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

wait_for() {
  local needle="$1" timeout="$2" waited=0
  while ! grep -a -q "$needle" "$SERIAL" 2>/dev/null; do
    sleep 2
    waited=$((waited + 2))
    if [ "$waited" -ge "$timeout" ]; then
      echo "[test] TIMEOUT waiting for '$needle'"
      return 1
    fi
  done
  return 0
}

{
  type_word "exec evict_test.elf"
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

PASS=1
if ! wait_for "\[evict\] PASS" 60; then
  echo "[test] FAIL: evict_test did not pass"
  PASS=0
fi

sleep 2
echo "quit" | timeout 5 socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true
sleep 2
kill -9 "$QPID" 2>/dev/null || true
for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done

NMAPPED=$(grep -a -c "\[pf\] demand-mapped" "$SERIAL" 2>/dev/null || echo 0)
if [ "$NMAPPED" -ne 17 ]; then
  echo "[test] FAIL: expected 17 demand-mapped, got $NMAPPED"
  PASS=0
fi

NEVICT=$(grep -a -c "\[pf\] evicting" "$SERIAL" 2>/dev/null || echo 0)
if [ "$NEVICT" -ne 13 ]; then
  echo "[test] FAIL: expected 13 evictions, got $NEVICT"
  PASS=0
fi

if grep -a -q "halting" "$SERIAL" 2>/dev/null; then
  echo "[test] FAIL: kernel halted"
  PASS=0
fi

if [ "$PASS" -eq 1 ]; then
  echo "[test] PASS: 17 faults, 13 FIFO evictions, no halt"
  exit 0
else
  echo "[test] FAIL"
  exit 1
fi
