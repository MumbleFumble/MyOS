#!/usr/bin/env bash
# Headless test for Phase 6 increment 1: ATA write path.
# Boots MyOS, snapshots wstat, runs wtest (writes 8 pattern sectors
# to the scratch area at the end of the disk and reads them back
# byte-for-byte), snapshots again. Asserts:
#   1. wtest reports write/readback OK
#   2. dma_write_sectors grows by >= 8 (writes really went over DMA)
#   3. dma_write_errors stays 0
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_write_serial.txt
SOCK=/tmp/qemu_write_mon.sock

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

printf '%s\n' "wstat" | send_cmds
sleep 2
printf '%s\n' "exec wtest.elf" | send_cmds
wait_for_count "wtest:" 1 120 || fail "wtest did not run"

grep -a -q "wtest: write/readback OK" "$SERIAL" || fail "wtest verification failed"

printf '%s\n' "wstat" | send_cmds
wait_for_count "dma_write_sectors=" 2 60 || fail "wstat output missing"

FIRST=$(grep -a -o "dma_write_sectors=[0-9]*" "$SERIAL" | head -1 | sed 's/dma_write_sectors=//')
LASTLINE=$(grep -a "dma_write_sectors=" "$SERIAL" | tail -1)
LAST=$(echo "$LASTLINE" | grep -o "dma_write_sectors=[0-9]*" | sed 's/dma_write_sectors=//')
ERRS=$(echo "$LASTLINE" | grep -o "dma_write_errors=[0-9]*" | sed 's/dma_write_errors=//')
DELTA=$(( ${LAST:-0} - ${FIRST:-0} ))
echo "[test] dma_write_sectors first=${FIRST:-missing} last=${LAST:-missing} delta=$DELTA dma_write_errors=${ERRS:-missing}"
[ "$DELTA" -ge 8 ] || fail "DMA wrote only $DELTA sectors across wtest"
[ "${ERRS:-999}" -eq 0 ] || fail "dma_write_errors=$ERRS during wtest"

echo "[test] PASS: ATA write path working (PIO + DMA write, readback verified)"
kill -9 "$QPID" 2>/dev/null
exit 0
