#!/usr/bin/env bash
# Collect a demand-access trace for prefetch-policy training data.
# Usage: collect_access_trace.sh <bench_cpu|bench_io|bench_mix> <out_serial>
# Boots MyOS with the default (none) prefetch policy, runs the benchmark,
# then dumps `acctrace` to the serial log. The ACC lines are the demand
# stream replayed offline by scripts/sim_prefetch.py.
set -u

BENCH="$1"        # bench_cpu | bench_io | bench_mix
SERIAL="$2"

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SOCK=/tmp/qemu_collect_acc_mon.sock

case "$BENCH" in
  bench_cpu) TIMEOUT=180 ;;
  bench_io)  TIMEOUT=420 ;;
  bench_mix) TIMEOUT=300 ;;
  *) echo "unknown benchmark: $BENCH" >&2; exit 1 ;;
esac

# Kill any stale QEMU first (a wedged instance holds the disk image lock).
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
echo "[collect] QEMU pid=$QPID bench=$BENCH (access trace)"

for i in $(seq 1 40); do [ -S "$SOCK" ] && break; sleep 0.5; done
sleep 20  # boot

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
    sleep 5
    waited=$((waited + 5))
    if [ "$waited" -ge "$timeout" ]; then
      echo "[collect] TIMEOUT waiting for '$needle'"
      return 1
    fi
  done
  echo "[collect] saw '$needle' after ~${waited}s"
  return 0
}

{
  type_word "exec $BENCH.elf"
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

if ! wait_for "$BENCH: checksum=" "$TIMEOUT"; then
  echo "[collect] benchmark did not finish"
fi
sleep 2

{
  type_word "acctrace"
  sleep 8
  echo "quit"
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

sleep 2
kill -9 "$QPID" 2>/dev/null || true
for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done

NREC=$(grep -a -c "ACC seq=" "$SERIAL" 2>/dev/null || echo 0)
echo "[collect] done: $NREC ACC lines in $SERIAL"
grep -a "$BENCH:" "$SERIAL" | tail -3
