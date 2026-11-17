#!/usr/bin/env bash
# Headless test for Phase 4 increment 1: IRQ-driven ATA waits.
# Boots MyOS and runs bench_io once. Asserts:
#   1. bench_io completes (the IRQ wait path does not hang)
#   2. the full 10 MiB is read (bytes=10485760)
#   3. bench_io reports io_blocks > 0 (the reading task really blocked
#      on I/O; with the old polling driver this was always 0)
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_atairq_serial.txt
SOCK=/tmp/qemu_atairq_mon.sock

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

printf '%s\n' "exec bench_io.elf" | send_cmds
wait_for_count "bench_io: bytes=" 1 240 || fail "bench_io did not finish (IRQ wait hang?)"

grep -a -q "bench_io: bytes=10485760" "$SERIAL" || fail "byte count wrong"
IOB=$(grep -a -o "io_blocks=[0-9]*" "$SERIAL" | tail -1 | sed 's/io_blocks=//')
echo "[test] bench_io io_blocks=${IOB:-missing}"
[ -n "$IOB" ] && [ "$IOB" -gt 0 ] || fail "io_blocks not > 0 (task never blocked on I/O)"

echo "[test] PASS: IRQ-driven ATA waits working (task blocks on I/O)"
kill -9 "$QPID" 2>/dev/null
exit 0
