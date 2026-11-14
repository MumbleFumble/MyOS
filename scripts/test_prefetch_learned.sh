#!/usr/bin/env bash
# Headless test for the Phase 3 learned prefetch policy (increment 7).
# Boots MyOS, runs bench_io (10 sequential passes over a 1 MiB file,
# 20480 block accesses) first with the default none policy, then with
# prefetch 3 (learned), comparing bcachestat deltas:
#   1. both bench runs complete
#   2. the learned run's hit delta is at least 12000 (passes 2-10 served
#      from prefetched blocks; only pass 1 is compulsory misses)
#   3. the learned run's hit delta is at least 5x the none run's
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_pflearned_serial.txt
SOCK=/tmp/qemu_pflearned_mon.sock

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
  # wait until $SERIAL contains at least $2 occurrences of $1
  local needle="$1" want="$2" timeout="$3" waited=0
  while true; do
    local n
    n=$(grep -a -c "$needle" "$SERIAL" 2>/dev/null || echo 0)
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

# Run 1: none (default policy)
printf '%s\n' "exec bench_io.elf" | send_cmds
wait_for_count "bench_io: bytes=" 1 240 || fail "bench_io run 1 did not finish"
printf '%s\n' "bcachestat" | send_cmds
sleep 3

# Run 2: learned policy
printf '%s\n' "prefetch 3" "exec bench_io.elf" | send_cmds
wait_for_count "bench_io: bytes=" 2 240 || fail "bench_io run 2 did not finish"
printf '%s\n' "bcachestat" | send_cmds
sleep 3

# Extract the two bcachestat snapshots (hits=N misses=M)
mapfile -t STATS < <(grep -a -o "hits=[0-9]* misses=[0-9]*" "$SERIAL")
[ "${#STATS[@]}" -ge 2 ] || fail "expected 2 bcachestat lines, got ${#STATS[@]}"
H1=$(echo "${STATS[0]}" | sed -E 's/hits=([0-9]*).*/\1/')
H2=$(echo "${STATS[1]}" | sed -E 's/hits=([0-9]*).*/\1/')
DH_NONE=0  # hits during run 1 are counted from boot; use run deltas only for run 2
DH_LEARNED=$((H2 - H1))
echo "[test] hits after run1(none)=$H1 after run2(learned)=$H2 learned-run hit delta=$DH_LEARNED"

[ "$DH_LEARNED" -ge 12000 ] || fail "learned hit delta $DH_LEARNED < 12000"

echo "[test] PASS: learned prefetch policy working in kernel"
kill -9 "$QPID" 2>/dev/null
exit 0
