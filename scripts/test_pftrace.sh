#!/usr/bin/env bash
# Headless test for Phase 2 fault telemetry (increment 3).
# Boots MyOS, runs demand_test (4 faults), then `pftrace` and checks:
#   1. pftrace reports "drained 4 fault records"
#   2. all 4 records show resolved=1
#   3. CR2 values are the 4 demand pages (0x20000000000, +0x1000, +0x2000, +0x3000)
#   4. pids are non-zero (attributed to the test task)
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_pftrace_serial.txt
SOCK=/tmp/qemu_pftrace_mon.sock

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
  type_word "exec demand_test.elf"
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

if ! wait_for "\[demand\] PASS" 60; then
  echo "[test] FAIL: demand_test did not pass"
  kill -9 "$QPID" 2>/dev/null; exit 1
fi
sleep 1

{
  type_word "pftrace"
  sleep 6
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

sleep 2
echo "quit" | timeout 5 socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true
sleep 2
kill -9 "$QPID" 2>/dev/null || true
for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done

PASS=1
N=$(grep -a -c "drained 4 fault records" "$SERIAL" 2>/dev/null || echo 0)
if [ "$N" -eq 0 ]; then
  echo "[test] FAIL: expected 'drained 4 fault records'"
  PASS=0
fi

# Check all 4 records have resolved=1.
NR=$(grep -a "resolved=1" "$SERIAL" | grep -c "cr2=" || echo 0)
if [ "$NR" -ne 4 ]; then
  echo "[test] FAIL: expected 4 records with resolved=1, got $NR"
  PASS=0
fi

# Check CR2 values.
for addr in "0000020000000000" "0000020000001000" "0000020000002000" "0000020000003000"; do
  if ! grep -a -q "cr2=0x$addr" "$SERIAL" 2>/dev/null; then
    echo "[test] FAIL: missing cr2=0x$addr"
    PASS=0
  fi
done

# Check pids non-zero.
if grep -a "cr2=" "$SERIAL" | grep -q "pid=0"; then
  echo "[test] FAIL: found pid=0 in fault records"
  PASS=0
fi

if [ "$PASS" -eq 1 ]; then
  echo "[test] PASS: pftrace shows 4 resolved faults with correct addresses and pids"
  exit 0
else
  echo "[test] FAIL"
  grep -a "cr2=" "$SERIAL" | tail -5
  exit 1
fi
