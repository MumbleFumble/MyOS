#!/usr/bin/env bash
# Evaluate a scheduling policy on bench_mix: boot, set policy, run benchmark,
# capture trace immediately on completion.
# Usage: eval_policy.sh <policy 0|1|2> <out_serial>
set -u

POLICY="$1"
SERIAL="$2"

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SOCK=/tmp/qemu_eval_mon.sock

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
  local str="$1" i c
  for (( i=0; i<${#str}; i++ )); do
    c="${str:$i:1}"
    case "$c" in
      ' ') echo "sendkey spc" ;;
      '.') echo "sendkey dot" ;;
      '_') echo "sendkey shift-minus" ;;
      *)   echo "sendkey $c" ;;
    esac
    sleep 0.05
  done
  echo "sendkey ret"
  sleep 0.5
}

{ type_word "policy $POLICY"; sleep 1; type_word "exec bench_mix.elf"; sleep 1; } \
  | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

# Poll fast for completion, then trace immediately.
for i in $(seq 1 120); do
  if grep -a -q "bench_mix: checksum=" "$SERIAL" 2>/dev/null; then break; fi
  sleep 2
done
sleep 1
{ type_word "trace"; sleep 6; echo "quit"; } \
  | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

sleep 2
kill -9 "$QPID" 2>/dev/null || true
for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
echo "[eval] policy=$POLICY done"
grep -a "bench_mix:" "$SERIAL" | tail -2
