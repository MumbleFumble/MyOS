#!/usr/bin/env bash
# Headless test for Phase 13 inc.3: overwrite/truncate tail free.
#
# All boots use truncated 3073-sector images (append limit
# 1025, as in test_unlink_free.sh). One shell `write` appends
# three 1-sector records when superseding tombstone-style
# (create record + one record per sys_write in cmd_write),
# each superseding the previous.
#
# Boot A (write churn): filler image, 72 free sectors. The
# same file is rewritten 40 times. Tombstone-only growth is
# 3 sectors per write — 120 > 72, appends would fail from
# write 25. With rollback-before-append every superseded tail
# record is freed, so the whole churn leaves exactly ONE
# record for the file: fsck walks 4 records (README, test.txt,
# filler, w.txt), 4 live, 0 tombstones.
#
# Boot B (oracle): plain image. write a, write b, rewrite a
# (non-tail: tombstones), rewrite b (non-tail: tombstones),
# rewrite b again (tail: rolled back). fsck must walk exactly
# 6 records with 4 live files and 2 tombstones (tombstone-only
# superseding would walk 17). Contents read back in-guest.
#
# Boots C+D (create guard): write g, rm g (inc.2 tail
# rollback leaves the entry pointing at the append pointer),
# re-create g with `write g.txt zz` — the fcreate append lands
# exactly on the rolled-back slot and must not be tombstoned.
# A second boot on the same image (remount) must list g.txt
# and read back its content; fsck walks 3 records, 0
# tombstones. (Note: every shell create is followed by writes
# in the same command, whose own rollback supersedes the
# create record, so the listing assert validates the sequence
# end to end rather than the guard in isolation.)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
TMP="$(mktemp -d /tmp/myos_writefree.XXXXXX)"

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
mkimg() { # out, extra-files...
  local out="$1"; shift
  python3 "$ROOT/scripts/mkfs_myfs.py" "$out" \
      "$ROOT/disk_files/README.txt" "$ROOT/disk_files/test.txt" "$@" > /dev/null \
    || fail "mkfs failed for $out"
  truncate -s "$SMALL_BYTES" "$out"
}

# ---------- boot A: write churn ----------
dd if=/dev/zero of="$TMP/filler.bin" bs=512 count=949 2>/dev/null
mkimg "$TMP/churn.img" "$TMP/filler.bin"

{ for i in $(seq 1 39); do echo "write w.txt w$i"; done
  echo "write w.txt zzfinal"
  echo "cat w.txt"
} > "$TMP/churn_cmds.txt"

boot "$TMP/churn.img" "$TMP/sa.txt" "$TMP/ka.sock"
send_cmds "$TMP/ka.sock" "$TMP/churn_cmds.txt"
wait_count "$TMP/sa.txt" "written" 40 420 \
  || fail "churn: only $(count 'written' "$TMP/sa.txt")/40 writes completed"
[ "$(count 'write: failed' "$TMP/sa.txt")" -eq 0 ] || fail "churn: a write failed (append region exhausted?)"
# Typed input is not echoed to the serial console, so the
# payload appears exactly once — in the cat output.
[ "$(count 'zzfinal' "$TMP/sa.txt")" -eq 1 ] || fail "churn: final content did not read back"
shutdown

OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/churn.img")" \
  || fail "churn image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "4 records walked, 4 live files, 0 tombstones" \
  || fail "churn image counts wrong (rewrites not freeing?): $OUT"
echo "[test] churn image: $OUT"

# ---------- boot B: oracle ----------
mkimg "$TMP/oracle.img"
printf '%s\n' \
  "write a.txt aaaa" \
  "write b.txt bbbb" \
  "write a.txt a2a2" \
  "write b.txt b2b2" \
  "write b.txt b3b3" \
  "ls" \
  "cat a.txt" \
  "cat b.txt" \
  "cat test.txt" > "$TMP/oracle_cmds.txt"

boot "$TMP/oracle.img" "$TMP/sb.txt" "$TMP/kb.sock"
send_cmds "$TMP/kb.sock" "$TMP/oracle_cmds.txt" 1.5
sleep 5
[ "$(count 'a2a2' "$TMP/sb.txt")" -eq 1 ] || fail "oracle: non-tail-rewritten a.txt content wrong"
[ "$(count 'b3b3' "$TMP/sb.txt")" -eq 1 ] || fail "oracle: tail-rewritten b.txt content wrong"
grep -a -q "hello from disk" "$TMP/sb.txt" || fail "oracle: test.txt did not read back"
grep -a -q "README.txt" "$TMP/sb.txt" || fail "oracle: README.txt missing from listing"
shutdown

OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/oracle.img")" \
  || fail "oracle image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "6 records walked, 4 live files, 2 tombstones" \
  || fail "oracle image counts wrong: $OUT"
echo "[test] oracle image: $OUT"

# ---------- boots C+D: create guard + remount ----------
mkimg "$TMP/guard.img"
printf '%s\n' \
  "write g.txt gg" \
  "rm g.txt" \
  "write g.txt zz" > "$TMP/guard_cmds.txt"

boot "$TMP/guard.img" "$TMP/sc.txt" "$TMP/kc.sock"
send_cmds "$TMP/kc.sock" "$TMP/guard_cmds.txt" 1.5
sleep 3
[ "$(count 'rm: deleted' "$TMP/sc.txt")" -eq 1 ] || fail "guard: tail delete did not succeed"
shutdown

printf '%s\n' "ls" "cat g.txt" > "$TMP/remount_cmds.txt"
boot "$TMP/guard.img" "$TMP/sd.txt" "$TMP/kd.sock"
send_cmds "$TMP/kd.sock" "$TMP/remount_cmds.txt" 1.5
sleep 3
grep -a -q "zz" "$TMP/sd.txt" || fail "guard: g.txt content missing after remount (file lost?)"
grep -a -q "cat: not found: g.txt" "$TMP/sd.txt" && fail "guard: g.txt absent after remount"
shutdown

OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/guard.img")" \
  || fail "guard image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "3 records walked, 3 live files, 0 tombstones" \
  || fail "guard image counts wrong: $OUT"
echo "[test] guard image: $OUT"

echo "[test] PASS: rewrites free their superseded tail records (40-write churn in 72 free sectors); oracle counts exact; re-created file survives remount; non-tail tombstones intact"
cleanup
exit 0
