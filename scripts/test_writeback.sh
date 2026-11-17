#!/usr/bin/env bash
# Headless test for Phase 6 increment 2: write-back block cache.
# Three boots on the SAME disk image (no rebuild between boots):
#   boot 1: wtest writes 8 pattern sectors through the cache, NO sync,
#           then QEMU is killed. The data must die with the cache.
#   boot 2: wtest verify must FAIL (proves the writes were cache-only);
#           then wtest writes again and the shell runs `sync`.
#   boot 3: wtest verify must PASS (sync made the writes durable).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SOCK=/tmp/qemu_wb_mon.sock
CUR_SERIAL=""

[ -f "$ISO" ]  || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }
[ -f "$DISK" ] || { echo "[test] FAIL: $DISK missing"; exit 1; }

# Hermetic disk: work on a private copy with the scratch area (last
# 2048 sectors) zeroed. Other tests sync patterns into the shared
# build/disk.img, which would otherwise make boot 2's verify pass
# vacuously.
WB_DISK=/tmp/myos_wb_disk.img
cp "$DISK" "$WB_DISK"
dd if=/dev/zero of="$WB_DISK" bs=512 seek=30720 count=2048 conv=notrunc status=none
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
  CUR_SERIAL="/tmp/myos_wb_serial_$1.txt"
  rm -f "$CUR_SERIAL" "$SOCK"
  qemu-system-x86_64 -m 256 -display none -vga none \
    -serial "file:$CUR_SERIAL" -monitor "unix:$SOCK,server,nowait" \
    -boot d -cdrom "$ISO" -drive "file=$DISK,format=raw,if=ide,index=0" &
  QPID=$!
  for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
  [ -S "$SOCK" ] || fail "monitor socket never appeared (boot $1)"
  sleep 20
}

echo "[test] boot 1: write patterns, no sync, kill"
boot_vm 1
printf '%s\n' "exec wtestw.elf" | send_cmds
wait_for_count "wtest: written" 1 120 || fail "boot1: wtest write did not run"
kill -9 "$QPID" 2>/dev/null; sleep 1

echo "[test] boot 2: verify must FAIL (cache-only data died), rewrite + sync"
boot_vm 2
printf '%s\n' "exec wtestv.elf" | send_cmds
wait_for_count "wtest: verify" 1 120 || fail "boot2: wtest verify did not run"
grep -a -q "wtest: verify FAIL" "$CUR_SERIAL" || fail "boot2: unsynced writes survived a reboot (not write-back?)"
printf '%s\n' "exec wtestw.elf" | send_cmds
wait_for_count "wtest: written" 1 120 || fail "boot2: wtest write did not run"
printf '%s\n' "sync" | send_cmds
wait_for_count "synced" 1 60 || fail "boot2: sync did not complete"
kill -9 "$QPID" 2>/dev/null; sleep 1

echo "[test] boot 3: verify must PASS (sync was durable)"
boot_vm 3
printf '%s\n' "exec wtestv.elf" | send_cmds
wait_for_count "wtest: verify" 1 120 || fail "boot3: wtest verify did not run"
grep -a -q "wtest: verify OK" "$CUR_SERIAL" || fail "boot3: synced writes did not persist"
kill -9 "$QPID" 2>/dev/null

echo "[test] PASS: write-back cache (dirty until sync, durable after)"
exit 0
