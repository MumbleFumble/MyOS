#!/usr/bin/env bash
# Headless test for Phase 13 inc.7: ordered writes + boot-time
# fsck repair (torn-tail recovery).
#
# Base image (truncated 3073-sector mkfs + one guest boot):
# README.txt, test.txt, a.txt, b.txt — 4 live records, append
# pointer at sector 5.
#
# Torn-tail copy: a garbage record (valid name, size 5, flag
# byte 0x07 — a partial header a crashed append could leave)
# is written at sector 5 and the superblock count is bumped
# to 5, exactly the state the pre-inc.7 write order could
# publish. fsck rejects it (tombstone-flag violation). The
# guest must mount it by truncating the torn tail, print the
# repair line, read the pre-crash files back, and persist the
# repair (fsck afterwards: CLEAN, 4 records). A write after
# the repair must survive a remount.
#
# Negative copy: record 1's flag byte is set to 0x07 (mid-log
# corruption, count untouched). The guest must NOT repair it:
# mount fails with the corrupt-record message, the disk stays
# unmounted, and the image on disk is left exactly as corrupt
# as it was (count and flag verified host-side afterwards).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
TMP="$(mktemp -d /tmp/myos_bootrepair.XXXXXX)"

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
python3 "$ROOT/scripts/mkfs_myfs.py" "$TMP/base.img" \
    "$ROOT/disk_files/README.txt" "$ROOT/disk_files/test.txt" > /dev/null \
  || fail "mkfs failed"
truncate -s "$SMALL_BYTES" "$TMP/base.img"

# ---------- boot 1: build the base state ----------
printf '%s\n' "write a.txt aaaa" "write b.txt bbbb" > "$TMP/cmds1.txt"
boot "$TMP/base.img" "$TMP/s1.txt" "$TMP/k1.sock"
send_cmds "$TMP/k1.sock" "$TMP/cmds1.txt" 1.5
wait_count "$TMP/s1.txt" "written" 2 120 || fail "boot 1: base writes did not complete"
shutdown
OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/base.img")" \
  || fail "base image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "4 records walked, 4 live files, 0 tombstones" \
  || fail "base image counts wrong: $OUT"
echo "[test] base: $OUT"

# ---------- construct the torn-tail and mid-log copies ----------
python3 - "$TMP/base.img" "$TMP/torn.img" "$TMP/mid.img" << 'PYEOF'
import struct, sys
base = open(sys.argv[1], "rb").read()
_, count = struct.unpack_from("<II", base, 0)
cur = 512
for _ in range(count):
    size = struct.unpack_from("<Q", base, cur + 64)[0]
    cur += ((80 + size + 511) // 512) * 512
append_sector = cur // 512

torn = bytearray(base)
hdr = bytearray(512)
hdr[0:8] = b"torn.txt"
struct.pack_into("<Q", hdr, 64, 5)   # plausible size
hdr[72] = 0x07                        # garbage flag byte: the tear
torn[cur:cur + 512] = hdr
struct.pack_into("<I", torn, 4, count + 1)   # count published over the tear
open(sys.argv[2], "wb").write(torn)

mid = bytearray(base)
# record 1 (test.txt) starts at sector 2; corrupt its flag byte
mid[2 * 512 + 72] = 0x07
open(sys.argv[3], "wb").write(mid)
print(f"[test] constructed: append sector {append_sector}, count {count} -> torn {count + 1}")
PYEOF

if python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/torn.img" > "$TMP/torn_fsck.txt" 2>&1; then
  fail "torn image unexpectedly passes fsck (construction broken)"
fi
grep -q "tombstone-flag" "$TMP/torn_fsck.txt" \
  || fail "torn image fails fsck for the wrong reason: $(cat "$TMP/torn_fsck.txt")"
echo "[test] torn image is fsck-rejected as expected: $(head -1 "$TMP/torn_fsck.txt")"

# ---------- boot 2: the torn image must mount via repair ----------
printf '%s\n' "cat a.txt" "cat test.txt" "df" > "$TMP/cmds2.txt"
boot "$TMP/torn.img" "$TMP/s2.txt" "$TMP/k2.sock"
send_cmds "$TMP/k2.sock" "$TMP/cmds2.txt" 1.5
wait_count "$TMP/s2.txt" "df: tombstones" 1 90 || fail "boot 2: df never printed (mount failed?)"
shutdown
grep -a -q "torn tail at record 4 of 5" "$TMP/s2.txt" \
  || fail "boot 2: repair message missing"
grep -a -q "\[serial\] diskfs mounted" "$TMP/s2.txt" \
  || fail "boot 2: kernel did not report a successful mount"
[ "$(count 'aaaa' "$TMP/s2.txt")" -ge 1 ] || fail "boot 2: a.txt content lost in repair"
[ "$(count 'hello from disk' "$TMP/s2.txt")" -ge 1 ] || fail "boot 2: test.txt content lost in repair"
tr -d '\r' < "$TMP/s2.txt" > "$TMP/s2c.txt"
FREE2=$(grep -a "df: capacity" "$TMP/s2c.txt" | head -1 | sed -E 's/.*free ([0-9]+).*/\1/')
[ "$FREE2" = 1020 ] || fail "boot 2: df free $FREE2 != 1020 (append pointer not restored to sector 5)"
OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/torn.img")" \
  || fail "repaired image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "4 records walked, 4 live files, 0 tombstones" \
  || fail "repaired image counts wrong (repair not persisted?): $OUT"
echo "[test] repaired: $OUT; guest mounted, pre-crash files intact, df free $FREE2"

# ---------- boots 3+4: post-repair write survives a remount ----------
printf '%s\n' "write c.txt cccc" > "$TMP/cmds3.txt"
boot "$TMP/torn.img" "$TMP/s3.txt" "$TMP/k3.sock"
send_cmds "$TMP/k3.sock" "$TMP/cmds3.txt" 1.5
wait_count "$TMP/s3.txt" "written" 1 90 || fail "boot 3: post-repair write did not complete"
shutdown
printf '%s\n' "cat c.txt" "cat a.txt" > "$TMP/cmds4.txt"
boot "$TMP/torn.img" "$TMP/s4.txt" "$TMP/k4.sock"
send_cmds "$TMP/k4.sock" "$TMP/cmds4.txt" 1.5
wait_count "$TMP/s4.txt" "aaaa" 1 90 || fail "boot 4: a.txt did not read back after remount"
shutdown
[ "$(count 'cccc' "$TMP/s4.txt")" -ge 1 ] || fail "boot 4: c.txt (written after repair) lost on remount"
OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/torn.img")" \
  || fail "final torn image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "5 records walked, 5 live files, 0 tombstones" \
  || fail "final torn image counts wrong: $OUT"
echo "[test] post-repair write persisted: $OUT"

# ---------- negative: mid-log corruption must NOT be repaired ----------
printf '%s\n' "cat test.txt" > "$TMP/cmds5.txt"
boot "$TMP/mid.img" "$TMP/s5.txt" "$TMP/k5.sock"
send_cmds "$TMP/k5.sock" "$TMP/cmds5.txt" 1.5
wait_count "$TMP/s5.txt" "cat: not found" 1 90 || fail "boot 5: cat on unmounted disk gave no verdict"
shutdown
grep -a -q "record 1 of 4 is corrupt (not a torn tail), mount failed" "$TMP/s5.txt" \
  || fail "boot 5: corrupt-record message missing"
grep -a -q "\[serial\] diskfs: no disk found" "$TMP/s5.txt" \
  || fail "boot 5: kernel did not report the failed mount"
[ "$(count 'log truncated and superblock repaired' "$TMP/s5.txt")" -eq 0 ] || fail "boot 5: a repair was attempted on mid-log damage"
python3 - "$TMP/mid.img" << 'PYEOF' || fail "boot 5: the corrupt image was modified on disk (silent repair?)"
import struct, sys
d = open(sys.argv[1], "rb").read()
_, count = struct.unpack_from("<II", d, 0)
assert count == 4, f"superblock count changed: {count}"
assert d[2 * 512 + 72] == 0x07, "corrupt flag byte was rewritten"
print("[test] negative image on disk untouched: count still 4, flag still 0x07")
PYEOF

echo "[test] PASS: torn tail repaired at boot (files intact, fsck CLEAN, write survives remount); mid-log corruption refused loudly and left untouched"
cleanup
exit 0
