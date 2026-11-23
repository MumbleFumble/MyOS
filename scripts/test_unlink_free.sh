#!/usr/bin/env bash
# Headless test for Phase 13 inc.2: unlink free path.
#
# Both boots use a small custom image: mkfs output truncated to
# 3073 sectors, so the append limit (total - 2048, diskfs.c) is
# 1025 sectors and the arithmetic stays small.
#
# Boot 1 (churn): the image also embeds filler.bin, a 950-sector
# record, so the log starts at sector 953 with 72 sectors free.
# Each `write churn.txt` + `rm churn.txt` iteration appends
# THREE 1-sector records (the create record, then one record per
# sys_write in cmd_write: the text, then text + "\n"; each
# write_op tombstones the previous one) and the delete frees
# one. Tombstone-only delete would grow the log by 3 sectors
# per iteration — 30 iterations need 90 > 72, so appends would
# start failing at iteration 25. With the tail free path the
# final record is rolled back on every delete, growth is 2
# sectors per iteration (60 <= 72), and all 30 succeed. The
# post-boot fsck count proves it: 63 records walked (3 initial +
# 60 tombstones), not 93.
#
# Boot 2 (oracle): plain image (README.txt + test.txt). Each
# write appends 3 records (create + two write_ops), so after
# write o1 + write o2 the log holds 8 records. rm o2 frees its
# final record by rollback (7 left); rm o1 is non-tail by then
# and tombstones (still 7). fsck on the result must walk
# exactly 7 records with 2 live files and 5 tombstones, and
# the survivors must still read back byte-identical (README.txt
# is checked via the ls listing: the sendkey typer cannot send
# capital letters, so its name is never typed).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
TMP="$(mktemp -d /tmp/myos_unlink.XXXXXX)"

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

SMALL_BYTES=$((3073 * 512))

# ---------- boot 1: churn ----------
dd if=/dev/zero of="$TMP/filler.bin" bs=512 count=949 2>/dev/null
python3 "$ROOT/scripts/mkfs_myfs.py" "$TMP/churn.img" \
    "$ROOT/disk_files/README.txt" "$ROOT/disk_files/test.txt" \
    "$TMP/filler.bin" > /dev/null || fail "mkfs (churn image) failed"
truncate -s "$SMALL_BYTES" "$TMP/churn.img"

{ for i in $(seq 1 30); do
    echo "write churn.txt churn-$i"
    echo "rm churn.txt"
  done } > "$TMP/churn_cmds.txt"

boot "$TMP/churn.img" "$TMP/s1.txt" "$TMP/k1.sock"
send_cmds "$TMP/k1.sock" "$TMP/churn_cmds.txt"
wait_count "$TMP/s1.txt" "rm: deleted" 30 420 \
  || fail "churn: only $(count 'rm: deleted' "$TMP/s1.txt")/30 deletes completed"
[ "$(count 'written' "$TMP/s1.txt")" -ge 30 ] || fail "churn: fewer than 30 writes succeeded"
[ "$(count 'write: failed' "$TMP/s1.txt")" -eq 0 ] || fail "churn: a write failed (append region exhausted?)"
shutdown

OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/churn.img")" \
  || fail "churn image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "63 records walked, 3 live files, 60 tombstones" \
  || fail "churn image counts wrong (rollback not freeing?): $OUT"
echo "[test] churn image: $OUT"

# ---------- boot 2: oracle ----------
python3 "$ROOT/scripts/mkfs_myfs.py" "$TMP/oracle.img" \
    "$ROOT/disk_files/README.txt" "$ROOT/disk_files/test.txt" > /dev/null \
  || fail "mkfs (oracle image) failed"
truncate -s "$SMALL_BYTES" "$TMP/oracle.img"

printf '%s\n' \
  "write o1.txt hello-o1" \
  "write o2.txt hello-o2" \
  "rm o2.txt" \
  "rm o1.txt" \
  "ls" \
  "cat test.txt" \
  "cat o1.txt" \
  "cat o2.txt" > "$TMP/oracle_cmds.txt"

boot "$TMP/oracle.img" "$TMP/s2.txt" "$TMP/k2.sock"
send_cmds "$TMP/k2.sock" "$TMP/oracle_cmds.txt" 1.5
sleep 5
[ "$(count 'rm: deleted' "$TMP/s2.txt")" -eq 2 ] || fail "oracle: expected 2 successful deletes"
grep -a -q "README.txt" "$TMP/s2.txt" || fail "oracle: README.txt missing from the post-delete listing"
grep -a -q "hello from disk" "$TMP/s2.txt" || fail "oracle: test.txt did not read back"
grep -a -q "cat: not found: o1.txt" "$TMP/s2.txt" || fail "oracle: o1.txt still visible after delete"
grep -a -q "cat: not found: o2.txt" "$TMP/s2.txt" || fail "oracle: o2.txt still visible after delete"
shutdown

OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/oracle.img")" \
  || fail "oracle image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "7 records walked, 2 live files, 5 tombstones" \
  || fail "oracle image counts wrong (expected the tail rollback to remove one record): $OUT"
echo "[test] oracle image: $OUT"

echo "[test] PASS: tail delete frees and reuses space (30 churn cycles, 90 sectors of appends in 72 free sectors); fsck counts exact; non-tail tombstone path intact"
cleanup
exit 0
