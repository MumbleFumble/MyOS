#!/usr/bin/env bash
# Headless test for Phase 5 increment 1: bus-master DMA reads.
# Boots MyOS, snapshots dmastat, runs bench_io, snapshots again.
# Asserts:
#   1. bench_io completes (bytes=10485760)
#   2. dma_sectors grows by >= 20000 across the run (the workload
#      really moved over DMA, not the PIO fallback)
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_dma_serial.txt
SOCK=/tmp/qemu_dma_mon.sock

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

send_cmds() {
  { while IFS= read -r line; do type_word "$line"; sleep 1; done; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true
}

wait_for_count() {
  local needle="$1" want="$2" timeout="$3" waited=0
  while true; do
    local n
    n=$(grep -a -c "$needle" "$SERIAL" 2>/dev/null || true); n=${n:-0}
    [ "${n:-0}" -ge "$want" ] && return 0
    sleep 5
    waited=$((waited + 5))
    if [ "$waited" -ge "$timeout" ]; then
      echo "[test] TIMEOUT waiting for $want x '$needle' (saw $n)"
      return 1
    fi
  done
}

fail() { echo "[test] FAIL: $1"; kill -9 "$QPID" 2>/dev/null; exit 1; }

printf '%s\n' "dmastat" | send_cmds
sleep 2
printf '%s\n' "exec bench_io.elf" | send_cmds
wait_for_count "bench_io: bytes=" 1 240 || fail "bench_io did not finish (DMA hang?)"

grep -a -q "bench_io: bytes=10485760" "$SERIAL" || fail "byte count wrong"

printf '%s\n' "dmastat" | send_cmds
wait_for_count "dma_sectors=" 2 60 || fail "dmastat output missing"

FIRST=$(grep -a -o "dma_sectors=[0-9]*" "$SERIAL" | head -1 | sed 's/dma_sectors=//')
LASTLINE=$(grep -a "dma_sectors=" "$SERIAL" | tail -1)
LAST=$(echo "$LASTLINE" | grep -o "dma_sectors=[0-9]*" | sed 's/dma_sectors=//')
ERRS=$(echo "$LASTLINE" | grep -o "dma_errors=[0-9]*" | sed 's/dma_errors=//')
DELTA=$(( ${LAST:-0} - ${FIRST:-0} ))
echo "[test] dma_sectors first=${FIRST:-missing} last=${LAST:-missing} delta=$DELTA dma_errors=${ERRS:-missing}"
[ "$DELTA" -ge 20000 ] || fail "DMA carried only $DELTA sectors across bench_io"
[ "${ERRS:-999}" -eq 0 ] || fail "dma_errors=$ERRS during bench_io"

echo "[test] PASS: bus-master DMA reads working"
kill -9 "$QPID" 2>/dev/null
exit 0
