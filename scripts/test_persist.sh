#!/usr/bin/env bash
# Headless test for Phase 6 increment 3: MyFS file writes.
# Three boots on the SAME disk image (no rebuild between boots):
#   boot 1: shell `write note.txt hello persistence`, then `sync`.
#   boot 2: `cat note.txt` must show the text (it persisted); then
#           `append note.txt second line` and `sync`.
#   boot 3: `cat note.txt` must show BOTH lines (the append went
#           through the tombstone + append-record path and the mount
#           walk picked the latest live record).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SOCK=/tmp/qemu_persist_mon.sock
CUR_SERIAL=""

[ -f "$ISO" ]  || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }
[ -f "$DISK" ] || { echo "[test] FAIL: $DISK missing"; exit 1; }

# Hermetic disk: work on a private copy so the boots share one image
# (persistence across kills) without polluting the shared build image
# with a leftover note.txt.
WB_DISK=/tmp/myos_persist_disk.img
cp "$DISK" "$WB_DISK"
DISK="$WB_DISK"

type_word() {
  local w="$1"
  for ((i=0; i<${#w}; i++)); do
    local c="${w:i:1}"
    case "$c" in
      [a-z]) k="$c" ;;
      [0-9]) k="$c" ;;
      ' ') k="spc" ;;
      '.') k="dot" ;;
      '-') k="minus" ;;
      '_') k="shift-minus" ;;
      *) echo "[test] no key mapping for '$c'" >&2; return 1 ;;
    esac
    printf 'sendkey %s\n' "$k" | socat - "UNIX-CONNECT:$SOCK" >/dev/null
    sleep 0.06
  done
}

send_cmds() {
  local line
  while IFS= read -r line; do
    type_word "$line"
    printf 'sendkey ret\n' | socat - "UNIX-CONNECT:$SOCK" >/dev/null
    sleep 0.3
  done
}

wait_for_count() {
  local pat="$1" want="$2" timeout="$3" t=0 n=0
  while [ "$t" -lt "$timeout" ]; do
    n=$(grep -a -c "$pat" "$CUR_SERIAL" 2>/dev/null || true)
    [ "${n:-0}" -ge "$want" ] && return 0
    sleep 1; t=$((t+1))
  done
  echo "[test] TIMEOUT waiting for $want x '$pat' (saw ${n:-0})"
  return 1
}

fail() { echo "[test] FAIL: $1"; kill -9 "${QPID:-0}" 2>/dev/null; exit 1; }

boot_vm() {
  CUR_SERIAL="/tmp/myos_persist_serial_$1.txt"
  rm -f "$CUR_SERIAL" "$SOCK"
  qemu-system-x86_64 -m 256 -display none -vga none \
    -serial "file:$CUR_SERIAL" -monitor "unix:$SOCK,server,nowait" \
    -boot d -cdrom "$ISO" -drive "file=$DISK,format=raw,if=ide,index=0" &
  QPID=$!
  for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
  [ -S "$SOCK" ] || fail "monitor socket never appeared (boot $1)"
  sleep 20
}

echo "[test] boot 1: write note.txt + sync"
boot_vm 1
printf '%s\n' "write note.txt hello persistence" | send_cmds
wait_for_count "written" 1 60 || fail "boot1: write did not complete"
printf '%s\n' "sync" | send_cmds
wait_for_count "synced" 1 60 || fail "boot1: sync did not complete"
kill -9 "$QPID" 2>/dev/null; sleep 1

echo "[test] boot 2: cat must show the text; append + sync"
boot_vm 2
printf '%s\n' "cat note.txt" | send_cmds
wait_for_count "hello persistence" 1 60 || fail "boot2: written file did not persist"
printf '%s\n' "append note.txt second line" | send_cmds
wait_for_count "appended" 1 60 || fail "boot2: append did not complete"
printf '%s\n' "sync" | send_cmds
wait_for_count "synced" 1 60 || fail "boot2: sync did not complete"
kill -9 "$QPID" 2>/dev/null; sleep 1

echo "[test] boot 3: cat must show both lines"
boot_vm 3
printf '%s\n' "cat note.txt" | send_cmds
wait_for_count "second line" 1 60 || fail "boot3: appended line missing"
grep -a -q "hello persistence" "$CUR_SERIAL" || fail "boot3: original line lost in append"
kill -9 "$QPID" 2>/dev/null

echo "[test] PASS: MyFS writes persist across reboots (create + append)"
exit 0
