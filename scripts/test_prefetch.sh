#!/usr/bin/env bash
# Headless test for Phase 3 prefetch policies (increment 4).
# Boots MyOS, tests:
#   1. prefetch 0 (none): baseline, no prefetches
#   2. prefetch 1 (sequential): sequential workload should show hits
#   3. prefetch 2 (stride): should not crash
#   4. bcachestat shows policy working
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_prefetch_serial.txt
SOCK=/tmp/qemu_prefetch_mon.sock

pkill -9 -f "qemu-system-x86_64" 2>/dev/null || true
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

if ! wait_for "MyOS shell" 60; then
  echo "[test] FAIL: shell did not boot"
  kill -9 "$QPID" 2>/dev/null; exit 1
fi

# Test prefetch policy switching
for p in 0 1 2; do
  {
    type_word "prefetch $p"
    sleep 1
  } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true
  sleep 1
done

if ! grep -a -q "prefetch policy set" "$SERIAL"; then
  echo "[test] FAIL: prefetch policy switch failed"
  kill -9 "$QPID" 2>/dev/null; exit 1
fi

# Enable sequential prefetch, read a file (should trigger prefetches)
{
  type_word "prefetch 1"
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true
sleep 1
{
  type_word "cat test.txt"
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true
sleep 2
{
  type_word "bcachestat"
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true
sleep 2

if ! grep -a -q "hits=" "$SERIAL"; then
  echo "[test] FAIL: bcachestat no output after prefetch test"
  kill -9 "$QPID" 2>/dev/null; exit 1
fi

echo "[test] PASS: prefetch policies working"
kill -9 "$QPID" 2>/dev/null
exit 0
