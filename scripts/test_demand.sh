#!/usr/bin/env bash
# Headless test for Phase 2 demand paging (increment 2).
# Boots MyOS in QEMU (no display), runs `exec demand_test.elf` in the
# shell, and checks:
#   1. the program prints "[demand] PASS" (4 pages written + verified)
#   2. exactly 4 "[pf] demand-mapped" lines appear (one per first touch)
#   3. no "halting" line appears (the kernel never gave up)
#   4. the mapped pages are the expected ones (0x20000000000 .. +3)
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img
SERIAL=/tmp/myos_demand_serial.txt
SOCK=/tmp/qemu_demand_mon.sock

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
  type_word "exec demand_test.elf"
  sleep 1
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

PASS=1
if ! wait_for "\[demand\] PASS" 60; then
  echo "[test] FAIL: demand_test never reported PASS"
  PASS=0
fi

# Exactly one demand-map per touched page (4), none for the verify pass.
MAPPED=$(grep -c "demand-mapped page" "$SERIAL" 2>/dev/null || true)
if [ "$MAPPED" != "4" ]; then
  echo "[test] FAIL: expected 4 demand-mapped pages, saw $MAPPED"
  PASS=0
fi

# The mapped pages must be the 4 consecutive pages at DEMAND_BASE.
for p in 0 1 2 3; do
  page=$(printf "0x%016X" $((0x20000000000 + p * 4096)))
  if ! grep -q "demand-mapped page $page" "$SERIAL"; then
    echo "[test] FAIL: missing demand-map for page $page"
    PASS=0
  fi
done

if grep -q "halting" "$SERIAL"; then
  echo "[test] FAIL: kernel halted during demand test"
  PASS=0
fi

{
  echo "quit"
} | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

wait "$QPID" 2>/dev/null || true

echo ""
echo "=== [pf]/[demand] LINES ==="
grep -a "\[pf\]\|\[demand\]" "$SERIAL" | head -20
echo ""
if [ "$PASS" -eq 1 ]; then
  echo "[test] PASS: 4 demand pages faulted, mapped, verified"
else
  echo "[test] FAIL"
  echo "=== FULL SERIAL TAIL ==="
  tail -c 3000 "$SERIAL"
fi
exit $((1 - PASS))
