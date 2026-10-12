#!/usr/bin/env bash
# Headless test for the bench_io benchmark.
# Boots MyOS in QEMU (no display), types `exec bench_io.elf` via the
# QEMU monitor, waits for the benchmark to finish, then drains the
# switch-trace with `trace` and dumps task stats with `sched`.
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_bench_io_serial.txt
SOCK=/tmp/qemu_bench_io_mon.sock

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
      # '_' is shift+minus on the emulated keyboard; QEMU has no
      # single 'underscore' key, so send it as a chord.
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
  type_word "exec bench_io.elf"
  # Bench runs to completion; we poll the serial log from outside
  # instead of sleeping a fixed amount. QEMU keeps running.
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

# Poll for benchmark completion from the serial log.
if ! wait_for "bench_io: checksum=" 420; then
  echo "[test] benchmark did not finish in 180s"
fi
sleep 1

{
  type_word "trace"
  sleep 1
  type_word "sched"
  sleep 1
  echo "quit"
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

wait "$QPID" 2>/dev/null || true

echo ""
echo "=== SERIAL OUTPUT (bench-relevant) ==="
grep -a "bench_io\|tick=\|drained\|PID\|STATE\|Trace\|trace" "$SERIAL" | head -60
echo ""
echo "=== FULL SERIAL TAIL ==="
tail -c 3000 "$SERIAL"
