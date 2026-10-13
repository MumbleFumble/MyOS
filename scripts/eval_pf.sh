#!/usr/bin/env bash
# Evaluation: bench_pf under FIFO vs learned eviction policy.
# Two separate QEMU boots (clean demand-region state each time).
# Expected (from sim_pf_search.py): FIFO ~188 faults, learned ~104.
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img

run_one() {
  local policy="$1"   # "fifo" or "learned"
  local serial="$2"
  local sock="$3"
  for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
  sleep 1
  rm -f "$serial" "$sock"

  qemu-system-x86_64 \
    -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=ide,index=0 \
    -boot d -m 512M \
    -serial file:"$serial" \
    -monitor unix:"$sock",server,nowait \
    -display none &
  local qpid=$!

  for i in $(seq 1 40); do [ -S "$sock" ] && break; sleep 0.5; done
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

  if [ "$policy" = "learned" ]; then
    { type_word "pfpolicy 1"; sleep 2; } | socat - UNIX-CONNECT:"$sock" >/dev/null 2>&1 || true
  fi
  { type_word "exec bench_pf.elf"; sleep 1; } | socat - UNIX-CONNECT:"$sock" >/dev/null 2>&1 || true

  local waited=0
  while ! grep -a -q "\[bench_pf\] DONE" "$serial" 2>/dev/null; do
    sleep 2; waited=$((waited+2))
    if [ "$waited" -ge 60 ]; then echo "TIMEOUT ($policy)"; break; fi
  done
  sleep 1
  { type_word "pftrace"; sleep 8; } | socat - UNIX-CONNECT:"$sock" >/dev/null 2>&1 || true
  sleep 2

  echo "quit" | timeout 5 socat - UNIX-CONNECT:"$sock" >/dev/null 2>&1 || true
  sleep 2
  kill -9 "$qpid" 2>/dev/null || true
  for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done

  grep -a "drained .* fault records" "$serial" | tail -1
}

echo "[eval] running FIFO..."
FIFO=$(run_one "fifo" /tmp/myos_pf_fifo.txt /tmp/qemu_pf_fifo.sock)
echo "[eval] FIFO: $FIFO"

echo "[eval] running learned..."
LEARNED=$(run_one "learned" /tmp/myos_pf_learned.txt /tmp/qemu_pf_learned.sock)
echo "[eval] learned: $LEARNED"

echo "[eval] done"
