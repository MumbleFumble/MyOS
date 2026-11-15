#!/usr/bin/env bash
# End-to-end timing evaluation for Phase 3 prefetch policies (inc.8).
# For each policy (0=none 1=sequential 2=stride 3=learned): fresh boot,
# set the policy, run bench_io (10 sequential passes over 1 MiB =
# 20480 block accesses), and record the benchmark's own tick delta plus
# bcachestat hits/misses. Prints a comparison table.
#
# Honest-metrics note (Claude design review, Oct 2026): prefetching on
# the synchronous PIO path cannot hide latency -- a prefetch is a disk
# read the caller waits for. Expect ticks to track total physical reads
# (misses + issued prefetches), not the hit rate. This run establishes
# that baseline before any async-prefetch work.
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img

run_policy() {
  local pol="$1"
  local name="$2"
  local serial=/tmp/myos_evalpf_${pol}_serial.txt
  local sock=/tmp/qemu_evalpf_${pol}_mon.sock

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
    local str="$1" i c
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

  {
    type_word "prefetch $pol"
    sleep 1
    type_word "exec bench_io.elf"
    sleep 1
  } | socat - UNIX-CONNECT:"$sock" >/dev/null 2>&1 || true

  local waited=0
  while ! grep -a -q "bench_io: bytes=" "$serial" 2>/dev/null; do
    sleep 5
    waited=$((waited + 5))
    if [ "$waited" -ge 300 ]; then
      echo "[eval] TIMEOUT policy=$name"
      kill -9 "$qpid" 2>/dev/null
      return 1
    fi
  done
  sleep 2

  {
    type_word "bcachestat"
    sleep 1
  } | socat - UNIX-CONNECT:"$sock" >/dev/null 2>&1 || true
  sleep 3

  kill -9 "$qpid" 2>/dev/null

  local ticks hits misses
  ticks=$(grep -a -o "bench_io: end   ticks=[0-9]* (+[0-9]*)" "$serial" | tail -1 | sed -E 's/.*\(\+([0-9]*)\).*/\1/')
  local stat
  stat=$(grep -a -o "hits=[0-9]* misses=[0-9]*" "$serial" | tail -1)
  hits=$(echo "$stat" | sed -E 's/hits=([0-9]*).*/\1/')
  misses=$(echo "$stat" | sed -E 's/.*misses=([0-9]*)/\1/')
  echo "$name ticks=${ticks:-?} hits=${hits:-?} misses=${misses:-?}"
}

echo "policy       result"
run_policy 0 none
run_policy 1 sequential
run_policy 2 stride
run_policy 3 learned
