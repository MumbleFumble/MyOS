#!/usr/bin/env bash
# Headless test for Phase 13 inc.8: cache invalidation on free.
#
# Honest scope note (also in the inc-008 report): a guest-level
# STALE-BYTE serve is not constructible in this codebase, with
# or without the fix — every reuse of a freed sector passes
# through bcache_write (which overwrites a cached entry in
# place) before the new extent is readable, demand reads are
# extent-bounded, and prefetch inserts never overwrite an
# existing entry. So this test is discriminating at the
# MECHANISM level: it observes the block cache directly through
# the SYS_BCACHE_PROBE syscall (shell `bcprobe`) and asserts
# that the tail file's sector is cached before the delete and
# NOT cached after the rollback frees it, while the neighbour
# record's sector survives (no over-invalidation). Without the
# fix the post-delete probe still reports valid=1. The
# functional flows (delete-then-recreate same name, overwrite
# supersede, remount persistence, fsck CLEAN) are asserted on
# top.
#
# Layout on the truncated 3073-sector image: README.txt at
# sector 1, test.txt at 2, a.txt at 3, t.txt at 4.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
TMP="$(mktemp -d /tmp/myos_cachefree.XXXXXX)"

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }
command -v qemu-system-x86_64 >/dev/null || { echo "[test] FAIL: qemu-system-x86_64 missing"; exit 1; }

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1

QPID=""
cleanup() { [ -n "$QPID" ] && kill -9 "$QPID" 2>/dev/null; rm -rf "$TMP"; }
fail() { echo "[test] FAIL: $1"; cleanup; exit 1; }

boot() { # image serial sock
  rm -f "$2" "$3"
  qemu-system-x86_64 -m 256 -display none -vga none \
    -serial "file:$2" -monitor "unix:$3,server,nowait" \
    -boot d -cdrom "$ISO" -drive "file=$1,format=raw,if=ide,index=0" \
    -netdev "socket,id=n0,listen=127.0.0.1:10000" \
    -device "e1000,netdev=n0,mac=52:54:00:12:34:56" &
  QPID=$!
  local i
  for i in $(seq 1 40); do [ -S "$3" ] && break; sleep 0.5; done
  sleep 20
}

type_word() {
  local str="$1" i c
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
send_cmds() { # sock, file, inter-line delay (default 0.7)
  { while IFS= read -r line; do type_word "$line"; sleep "${3:-0.7}"; done < "$2"; } \
    | socat - UNIX-CONNECT:"$1" >/dev/null 2>&1 || true
}
count() { grep -a -c "$1" "$2" 2>/dev/null || true; }
wait_count() { # serial, pattern, wanted, budget_secs
  local i
  for i in $(seq 1 "$4"); do
    [ "$(count "$2" "$1")" -ge "$3" ] && return 0
    sleep 1
  done
  return 1
}
shutdown() { kill -9 "$QPID" 2>/dev/null; QPID=""; sleep 1; }
probe_line() { grep -a "bcprobe: lba $1 " "$2" | tail -1 | tr -d '\r'; }

IMG="$TMP/disk.img"
python3 "$ROOT/scripts/mkfs_myfs.py" "$IMG" \
    "$ROOT/disk_files/README.txt" "$ROOT/disk_files/test.txt" > /dev/null \
  || fail "mkfs failed"
truncate -s $((3073 * 512)) "$IMG"

# ---------- boot 1: prime, free, probe, recreate, supersede ----------
printf '%s\n' \
  "write a.txt aaaa" \
  "write t.txt tttt" \
  "cat t.txt" \
  "cat t.txt" \
  "bcprobe 4" \
  "bcprobe 3" \
  "rm t.txt" \
  "bcprobe 4" \
  "bcprobe 3" \
  "write t.txt newt" \
  "cat t.txt" \
  "write u.txt uuuu" \
  "write u.txt vvvv" \
  "cat u.txt" \
  > "$TMP/cmds1.txt"
boot "$IMG" "$TMP/s1.txt" "$TMP/k1.sock"
send_cmds "$TMP/k1.sock" "$TMP/cmds1.txt" 1.5
wait_count "$TMP/s1.txt" "bcprobe: lba" 4 120 || fail "boot 1: probes did not all print"
wait_count "$TMP/s1.txt" "vvvv" 1 60 || fail "boot 1: superseded u.txt did not read back"
shutdown

P4_PRE="$(probe_line 4 "$TMP/s1.txt")"
# probe_line takes the LAST match; the pre-delete probe is the FIRST lba-4 line
P4_FIRST=$(grep -a "bcprobe: lba 4 " "$TMP/s1.txt" | head -1 | tr -d '\r')
P3_FIRST=$(grep -a "bcprobe: lba 3 " "$TMP/s1.txt" | head -1 | tr -d '\r')
echo "$P4_FIRST" | grep -q "valid=1" || fail "boot 1: t.txt sector 4 was not cached before the delete ($P4_FIRST)"
echo "$P4_FIRST" | grep -q "dirty=0" || fail "boot 1: t.txt sector 4 unexpectedly dirty before the delete ($P4_FIRST)"
echo "$P3_FIRST" | grep -q "valid=1" || fail "boot 1: a.txt sector 3 was not cached ($P3_FIRST)"
echo "$P4_PRE" | grep -q "valid=0" || fail "boot 1: t.txt sector 4 STILL CACHED after the rollback freed it ($P4_PRE)"
P3_LAST=$(grep -a "bcprobe: lba 3 " "$TMP/s1.txt" | tail -1 | tr -d '\r')
echo "$P3_LAST" | grep -q "valid=1" || fail "boot 1: a.txt sector 3 was wrongly invalidated by t.txt's free ($P3_LAST)"
[ "$(count 'newt' "$TMP/s1.txt")" -ge 1 ] || fail "boot 1: recreated t.txt did not read back its new content"
echo "[test] probes: before free [$P4_FIRST]; after free [$P4_PRE]; neighbour intact [$P3_LAST]"

OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$IMG")" \
  || fail "image not fsck-CLEAN after boot 1: $OUT"
echo "$OUT" | grep -q "5 records walked, 5 live files, 0 tombstones" \
  || fail "unexpected fsck counts after boot 1: $OUT"
echo "[test] fsck after boot 1: $OUT"

# ---------- boot 2: remount persistence ----------
printf '%s\n' "cat t.txt" "cat u.txt" > "$TMP/cmds2.txt"
boot "$IMG" "$TMP/s2.txt" "$TMP/k2.sock"
send_cmds "$TMP/k2.sock" "$TMP/cmds2.txt" 1.5
wait_count "$TMP/s2.txt" "vvvv" 1 90 || fail "boot 2: u.txt did not read back after remount"
shutdown
[ "$(count 'newt' "$TMP/s2.txt")" -ge 1 ] || fail "boot 2: t.txt content lost on remount"

echo "[test] PASS: freed tail sector provably evicted from the cache at the rollback (probe valid=1 -> valid=0), neighbour entry intact, recreate/supersede/remount all read the new bytes, fsck CLEAN"
cleanup
exit 0
