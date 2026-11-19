#!/usr/bin/env bash
# Headless test for Phase 11 inc.2: MyFS delete (rm) and rename (mv).
# Boot 1: write a file, rename it (old name gone, content follows),
#   delete it (gone), reuse the name, then set up a renamed file and
#   sync.
# Boot 2: the rename and the deletion must both have persisted.
set -u

ROOT=~/workspace/myos
ISO=$ROOT/build/myos.iso
DISK=$ROOT/build/disk.img

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1

QPID=""
SOCK=""
boot() {
  rm -f "$1" "$2"
  qemu-system-x86_64 \
    -cdrom "$ISO" \
    -drive file="$DISK",format=raw,if=ide,index=0 \
    -boot d -m 512M \
    -serial file:"$1" \
    -monitor unix:"$2",server,nowait \
    -display none &
  QPID=$!
  SOCK=$2
  for i in $(seq 1 40); do [ -S "$2" ] && break; sleep 0.5; done
  sleep 20
}

shutdown() { kill -9 "$QPID" 2>/dev/null; sleep 1; }

type_word() {
  local str="$1"
  local i c
  for (( i=0; i<${#str}; i++ )); do
    c="${str:$i:1}"
    case "$c" in
      ' ') echo "sendkey spc" ;;
      '.') echo "sendkey dot" ;;
      '_') echo "sendkey shift-minus" ;;
      '-') echo "sendkey minus" ;;
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

fail() { echo "[test] FAIL: $1"; shutdown; exit 1; }
need() { grep -a -q "$2" "$1" || fail "missing '$2' in $1"; }

S1=/tmp/myos_rm1_serial.txt; K1=/tmp/qemu_rm1_mon.sock
S2=/tmp/myos_rm2_serial.txt; K2=/tmp/qemu_rm2_mon.sock

boot "$S1" "$K1"
printf '%s\n' "write rmtest.txt hello-rm" "mv rmtest.txt rmnew.txt" "cat rmtest.txt" "cat rmnew.txt" | send_cmds
sleep 3
need "$S1" "mv: renamed"
need "$S1" "cat: not found: rmtest.txt"
need "$S1" "hello-rm"

printf '%s\n' "rm rmnew.txt" "cat rmnew.txt" "write rmnew.txt second-life" "cat rmnew.txt" | send_cmds
sleep 3
need "$S1" "rm: deleted"
need "$S1" "cat: not found: rmnew.txt"
need "$S1" "second-life"

printf '%s\n' "write persist.txt keepme" "mv persist.txt kept.txt" "rm rmtest2.txt" "sync" | send_cmds
sleep 3
need "$S1" "rm: failed"   # deleting a nonexistent file must fail
shutdown

boot "$S2" "$K2"
printf '%s\n' "cat kept.txt" "cat persist.txt" "cat rmnew.txt" | send_cmds
sleep 3
need "$S2" "keepme"
need "$S2" "cat: not found: persist.txt"
need "$S2" "second-life"
printf '%s\n' "rm rmnew.txt" "rm kept.txt" "sync" | send_cmds
sleep 2
shutdown

echo "[test] PASS: rm and mv work, persist across reboot, names reusable"
exit 0
