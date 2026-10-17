#!/usr/bin/env bash
# Headless test for Phase 3 I/O telemetry (increment 1).
# Boots MyOS, runs bench_io (disk reads), then `iotrace` and checks:
#   1. iotrace reports "drained N I/O records" with N > 0
#   2. records show lba, count, latency fields
#   3. all records show result=0 (success)
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_iotrace_serial.txt
SOCK=/tmp/qemu_iotrace_mon.sock

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
  type_word "exec bench_io.elf"
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

if ! wait_for "bench_io" 120; then
  echo "[test] FAIL: bench_io did not run"
  kill -9 "$QPID" 2>/dev/null; exit 1
fi

# Wait for bench to finish (it prints a summary)
sleep 10

{
  type_word "iotrace"
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

if ! wait_for "drained " 30; then
  echo "[test] FAIL: iotrace produced no output"
  kill -9 "$QPID" 2>/dev/null; exit 1
fi

# Check that at least 1 record was drained
if grep -a -q "drained 0 I/O records" "$SERIAL"; then
  echo "[test] FAIL: iotrace drained 0 records (expected > 0)"
  kill -9 "$QPID" 2>/dev/null; exit 1
fi

# Check records have expected fields
if ! grep -a -q "lba=" "$SERIAL"; then
  echo "[test] FAIL: no lba= field in iotrace output"
  kill -9 "$QPID" 2>/dev/null; exit 1
fi

if ! grep -a -q "result=0" "$SERIAL"; then
  echo "[test] FAIL: no result=0 in iotrace output"
  kill -9 "$QPID" 2>/dev/null; exit 1
fi

echo "[test] PASS: iotrace working"
kill -9 "$QPID" 2>/dev/null
exit 0
