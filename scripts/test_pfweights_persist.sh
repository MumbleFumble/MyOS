#!/usr/bin/env bash
# Headless test for Phase 10 inc.3: learned weights persist on disk.
# Boot 1: learned-online from zero weights learns bench_io, then
#   `pfweights save` writes pfweights.bin to the MyFS disk.
# Boot 2 (same disk image): selecting learned-online must seed the
#   weights from the saved file (bias equals the boot-1 value, not
#   the trained -1.500 and not 0), and bench_io must score a high hit
#   delta immediately, with no cold-start learning period.
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1

QPID=""
boot() {  # $1 = serial file, $2 = monitor sock
  rm -f "$1" "$2"
  qemu-system-x86_64 \
    -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=ide,index=0 \
    -boot d -m 512M \
    -serial file:"$1" \
    -monitor unix:"$2",server,nowait \
    -display none &
  QPID=$!
  for i in $(seq 1 40); do [ -S "$2" ] && break; sleep 0.5; done
  sleep 20
}

shutdown() { kill -9 "$QPID" 2>/dev/null; sleep 1; }

SOCK=""
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
  local file="$1" needle="$2" want="$3" timeout="$4" waited=0
  while true; do
    local n
    n=$(grep -a -c "$needle" "$file" 2>/dev/null || true)
    [ "${n:-0}" -ge "$want" ] && return 0
    sleep 5
    waited=$((waited + 5))
    if [ "$waited" -ge "$timeout" ]; then
      echo "[test] TIMEOUT waiting for $want x '$needle' in $file (saw ${n:-0})"
      return 1
    fi
  done
}

fail() { echo "[test] FAIL: $1"; shutdown; exit 1; }

S1=/tmp/myos_persist1_serial.txt; K1=/tmp/qemu_persist1_mon.sock
S2=/tmp/myos_persist2_serial.txt; K2=/tmp/qemu_persist2_mon.sock

# ---- Boot 1: learn from zero, save ----
SOCK=$K1
boot "$S1" "$K1"
printf '%s\n' "prefetch 4" "pfweights zero" | send_cmds
printf '%s\n' "exec bench_io.elf" | send_cmds
wait_for_count "$S1" "bench_io: bytes=" 1 300 || fail "boot1 bench_io did not finish"
printf '%s\n' "pfweights" "pfweights save" | send_cmds
sleep 3
grep -a -q "pfweights: saved" "$S1" || fail "pfweights save did not confirm"
B1=$(grep -a -o "bias=-*[0-9.]*" "$S1" | tail -1 | cut -d= -f2)
echo "[test] boot1 learned bias=$B1"
awk "BEGIN{exit !($B1 > 0.3)}" || fail "boot1 bias $B1 not learned-positive"
shutdown

# ---- Boot 2: weights must come back from disk ----
SOCK=$K2
boot "$S2" "$K2"
printf '%s\n' "prefetch 4" "pfweights" | send_cmds
sleep 3
B2=$(grep -a -o "bias=-*[0-9.]*" "$S2" | tail -1 | cut -d= -f2)
echo "[test] boot2 restored bias=$B2 (boot1: $B1, trained: -1.500)"
[ "$B1" = "$B2" ] || fail "restored bias $B2 != saved bias $B1"

printf '%s\n' "bcachestat" "exec bench_io.elf" | send_cmds
wait_for_count "$S2" "bench_io: bytes=" 1 300 || fail "boot2 bench_io did not finish"
printf '%s\n' "bcachestat" | send_cmds
sleep 3
mapfile -t STATS < <(grep -a -o "hits=[0-9]* misses=[0-9]*" "$S2")
[ "${#STATS[@]}" -ge 2 ] || fail "expected 2 bcachestat lines in boot2, got ${#STATS[@]}"
H1=$(echo "${STATS[0]}" | sed -E 's/hits=([0-9]*).*/\1/')
H2=$(echo "${STATS[1]}" | sed -E 's/hits=([0-9]*).*/\1/')
DH=$((H2 - H1))
echo "[test] boot2 hit delta with restored weights=$DH"
[ "$DH" -ge 8000 ] || fail "boot2 hit delta $DH < 8000; restored weights not effective"
shutdown

echo "[test] PASS: learned weights persisted across reboot and took effect immediately"
exit 0
