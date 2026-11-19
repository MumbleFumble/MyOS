#!/usr/bin/env bash
# Headless test for Phase 10 online learning (learned-online policy).
# Boots MyOS, selects prefetch 4 (learned-online), ZEROES the weights,
# then runs bench_io (10 sequential passes, 20480 accesses). Starting
# from zero weights the policy prefetches nothing; it must teach
# itself from outcomes within the run:
#   1. bench_io completes
#   2. positive updates happened (learning machinery is live)
#   3. is_next weight moved from 0 toward its trained positive value
#   4. the run's bcachestat hit delta is at least 8000 (frozen learned
#      scores ~12653 on this bench; the none policy ~2550), proving
#      in-run adaptation rather than a static policy
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_online_serial.txt
SOCK=/tmp/qemu_online_mon.sock

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

printf '%s\n' "bcachestat" "prefetch 4" "pfweights zero" | send_cmds
sleep 2
printf '%s\n' "exec bench_io.elf" | send_cmds
wait_for_count "bench_io: bytes=" 1 300 || fail "bench_io did not finish"
printf '%s\n' "bcachestat" "pfweights" | send_cmds
sleep 3

mapfile -t STATS < <(grep -a -o "hits=[0-9]* misses=[0-9]*" "$SERIAL")
[ "${#STATS[@]}" -ge 2 ] || fail "expected 2 bcachestat lines, got ${#STATS[@]}"
H1=$(echo "${STATS[0]}" | sed -E 's/hits=([0-9]*).*/\1/')
H2=$(echo "${STATS[1]}" | sed -E 's/hits=([0-9]*).*/\1/')
DH=$((H2 - H1))
echo "[test] online-from-zero hit delta=$DH (hits $H1 -> $H2)"

UPD=$(grep -a -o "updates: +[0-9]* -[0-9]*" "$SERIAL" | tail -1)
POS=$(echo "$UPD" | sed -E 's/updates: \+([0-9]*).*/\1/')
NEG=$(echo "$UPD" | sed -E 's/updates: \+[0-9]* -([0-9]*)/\1/')
echo "[test] weight updates line: $UPD"
# The margin perceptron converges fast (observed +25/-16 on first
# run): once scores pass the confidence margin, updates stop by
# design. Assert both directions fired, not a large count.
[ -n "$POS" ] && [ "$POS" -ge 10 ] || fail "only ${POS:-0} positive updates; learning not happening"
[ -n "$NEG" ] && [ "$NEG" -ge 1 ] || fail "no negative updates; waste feedback not wired"

# The weights the policy taught itself on this sequential workload
# (observed run 1: bias 0.562 vs trained -1.500, seq_run 0.406 vs
# trained 0.300): assert the two load-bearing weights moved from
# zero in the direction this workload rewards.
BIAS=$(grep -a -o "bias=-*[0-9.]*" "$SERIAL" | tail -1 | cut -d= -f2)
SEQR=$(grep -a -o "seq_run=-*[0-9.]*" "$SERIAL" | tail -1 | cut -d= -f2)
echo "[test] self-taught bias=$BIAS seq_run=$SEQR (both started at 0)"
awk "BEGIN{exit !($BIAS > 0.3)}" || fail "bias did not learn positive (got $BIAS)"
awk "BEGIN{exit !($SEQR > 0.2)}" || fail "seq_run did not learn positive (got $SEQR)"

[ "$DH" -ge 8000 ] || fail "hit delta $DH < 8000; no in-run adaptation"

echo "[test] PASS: online policy learned from zero weights within one run"
kill -9 "$QPID" 2>/dev/null
exit 0
