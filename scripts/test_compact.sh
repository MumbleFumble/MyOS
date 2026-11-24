#!/usr/bin/env bash
# Headless test for Phase 13 inc.6: offline compaction.
#
# Boot 1 builds the inc-005 state on a truncated 3073-sector
# image (write a, write b, rewrite a, rm b): 5 records walked,
# 3 live, 2 non-tail tombstones holding 2 dead sectors, and df
# is captured. Offline: fsck confirms the pre-state; a corrupt
# copy (zeroed magic) must be REFUSED by the tool with its
# bytes untouched; the real image is compacted and fsck must
# then walk 3 records / 3 live / 0 tombstones, with every live
# record's extent byte-identical to the pre-compaction image.
# Boot 2 (compacted image): df free must be the pre free plus
# exactly the reclaimed dead sectors, contents read back, and
# a new write appends. Boot 3: the post-compaction write and
# the old contents survive the remount; fsck still CLEAN.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
TMP="$(mktemp -d /tmp/myos_compact.XXXXXX)"

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

df_fields() { # serialfile -> sets d_free d_lrec d_trec d_deadsec (first df report)
  local clean="$TMP/df_clean.txt"
  tr -d '\r' < "$1" > "$clean"
  local cap_line live_line tomb_line
  cap_line=$(grep -a "df: capacity" "$clean" | head -1)
  live_line=$(grep -a "df: live" "$clean" | head -1)
  tomb_line=$(grep -a "df: tombstones" "$clean" | head -1)
  d_free=$(echo "$cap_line" | sed -E 's/.*free ([0-9]+).*/\1/')
  d_lrec=$(echo "$live_line" | sed -E 's/.*live ([0-9]+) records.*/\1/')
  d_trec=$(echo "$tomb_line" | sed -E 's/.*tombstones ([0-9]+) records, ([0-9]+) sectors.*/\1/')
  d_deadsec=$(echo "$tomb_line" | sed -E 's/.*tombstones ([0-9]+) records, ([0-9]+) sectors.*/\2/')
}

SMALL_BYTES=$((3073 * 512))
python3 "$ROOT/scripts/mkfs_myfs.py" "$TMP/disk.img" \
    "$ROOT/disk_files/README.txt" "$ROOT/disk_files/test.txt" > /dev/null \
  || fail "mkfs failed"
truncate -s "$SMALL_BYTES" "$TMP/disk.img"

# ---------- boot 1: build the state, capture pre figures ----------
printf '%s\n' \
  "write a.txt aaaa" \
  "write b.txt bbbb" \
  "write a.txt a2a2" \
  "rm b.txt" \
  "df" \
  "cat a.txt" \
  "cat test.txt" > "$TMP/cmds1.txt"
boot "$TMP/disk.img" "$TMP/s1.txt" "$TMP/k1.sock"
send_cmds "$TMP/k1.sock" "$TMP/cmds1.txt" 1.5
wait_count "$TMP/s1.txt" "df: tombstones" 1 90 || fail "boot 1: df never printed"
wait_count "$TMP/s1.txt" "hello from disk" 1 60 || fail "boot 1: test.txt did not read back"
shutdown
df_fields "$TMP/s1.txt"
pre_free=$d_free; pre_dead=$d_deadsec
[ "$d_lrec" = 3 ] && [ "$d_trec" = 2 ] || fail "boot 1: unexpected pre-state df (live=$d_lrec tomb=$d_trec)"
[ "$(count 'a2a2' "$TMP/s1.txt")" -ge 1 ] || fail "boot 1: a.txt content missing"
cp "$TMP/disk.img" "$TMP/pre.img"
OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/disk.img")" \
  || fail "pre image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "5 records walked, 3 live files, 2 tombstones" \
  || fail "pre image counts wrong: $OUT"
echo "[test] pre-compaction: $OUT; df free $pre_free, dead sectors $pre_dead"

# ---------- corrupt image: the tool must refuse, unmodified ----------
cp "$TMP/pre.img" "$TMP/corrupt.img"
python3 - "$TMP/corrupt.img" << 'PYEOF'
import sys
p = sys.argv[1]
d = bytearray(open(p, "rb").read())
d[0:4] = b"\x00\x00\x00\x00"   # zero the superblock magic
open(p, "wb").write(d)
PYEOF
if python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/corrupt.img" > /dev/null 2>&1; then
  fail "corrupt image unexpectedly passes fsck (test construction broken)"
fi
SUM_BEFORE=$(sha256sum "$TMP/corrupt.img" | cut -d' ' -f1)
if python3 "$ROOT/scripts/compact_myfs.py" "$TMP/corrupt.img" > "$TMP/corrupt_out.txt" 2>&1; then
  fail "tool accepted a corrupt image"
fi
grep -q "REFUSED" "$TMP/corrupt_out.txt" || fail "tool did not report a clean refusal: $(cat "$TMP/corrupt_out.txt")"
SUM_AFTER=$(sha256sum "$TMP/corrupt.img" | cut -d' ' -f1)
[ "$SUM_BEFORE" = "$SUM_AFTER" ] || fail "tool modified the corrupt image it refused"
echo "[test] corrupt image refused cleanly, bytes unmodified"

# ---------- compact the real image ----------
COUT="$(python3 "$ROOT/scripts/compact_myfs.py" "$TMP/disk.img")" \
  || fail "compaction failed: $COUT"
echo "[test] $COUT"
echo "$COUT" | grep -q "reclaimed $pre_dead sectors" \
  || fail "tool reclaimed != df dead sectors ($pre_dead): $COUT"
OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/disk.img")" \
  || fail "compacted image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "3 records walked, 3 live files, 0 tombstones" \
  || fail "compacted image counts wrong: $OUT"
echo "[test] post-compaction: $OUT"

# ---------- byte-identity of every live record ----------
python3 - "$TMP/pre.img" "$TMP/disk.img" << 'PYEOF' || fail "live records differ after compaction"
import struct, sys
def live_extents(path):
    data = open(path, "rb").read()
    _, count = struct.unpack_from("<II", data, 0)
    cur, live = 512, {}
    for _ in range(count):
        name = data[cur:cur + 64].split(b"\x00")[0].decode()
        size = struct.unpack_from("<Q", data, cur + 64)[0]
        flag = data[cur + 72]
        sects = (80 + size + 511) // 512
        if flag & 1:
            live.pop(name, None)
        else:
            live[name] = data[cur:cur + sects * 512]
        cur += sects * 512
    return live
pre, post = live_extents(sys.argv[1]), live_extents(sys.argv[2])
assert set(pre) == set(post), f"live file set changed: {set(pre)} vs {set(post)}"
for name in pre:
    assert pre[name] == post[name], f"record bytes differ for {name}"
print(f"[test] byte-identity: {len(pre)} live records identical pre/post compaction")
PYEOF

# ---------- boot 2: compacted image in the guest ----------
printf '%s\n' \
  "df" \
  "cat a.txt" \
  "cat test.txt" \
  "ls" \
  "write c.txt cccc" \
  "df" > "$TMP/cmds2.txt"
boot "$TMP/disk.img" "$TMP/s2.txt" "$TMP/k2.sock"
send_cmds "$TMP/k2.sock" "$TMP/cmds2.txt" 1.5
wait_count "$TMP/s2.txt" "df: tombstones" 2 120 || fail "boot 2: second df never printed"
shutdown
df_fields "$TMP/s2.txt"
want_free=$((pre_free + pre_dead))
[ "$d_free" = "$want_free" ] || fail "boot 2: df free $d_free != pre free + reclaimed ($want_free)"
[ "$d_trec" = 0 ] || fail "boot 2: df still shows tombstones ($d_trec)"
grep -a -q "README.txt" "$TMP/s2.txt" || fail "boot 2: README.txt missing from ls"
[ "$(count 'a2a2' "$TMP/s2.txt")" -ge 1 ] || fail "boot 2: a.txt content lost"
[ "$(count 'hello from disk' "$TMP/s2.txt")" -ge 1 ] || fail "boot 2: test.txt content lost"
echo "[test] boot 2: df free grew by exactly the reclaimed $pre_dead sectors ($pre_free -> $d_free)"

# ---------- boot 3: remount — the post-compaction write persisted ----------
printf '%s\n' \
  "cat c.txt" \
  "cat a.txt" > "$TMP/cmds3.txt"
boot "$TMP/disk.img" "$TMP/s3.txt" "$TMP/k3.sock"
send_cmds "$TMP/k3.sock" "$TMP/cmds3.txt" 1.5
wait_count "$TMP/s3.txt" "a2a2" 1 90 || fail "boot 3: a.txt did not read back after remount"
shutdown
[ "$(count 'cccc' "$TMP/s3.txt")" -ge 1 ] || fail "boot 3: c.txt (written after compaction) lost on remount"
OUT="$(python3 "$ROOT/scripts/fsck_myfs.py" "$TMP/disk.img")" \
  || fail "final image not fsck-CLEAN: $OUT"
echo "$OUT" | grep -q "4 records walked, 4 live files, 0 tombstones" \
  || fail "final image counts wrong: $OUT"
echo "[test] final: $OUT"

echo "[test] PASS: offline compaction reclaimed the tombstone space; fsck CLEAN with 0 tombstones; live contents byte-identical; post-compaction write survives remount; corrupt image refused unmodified"
cleanup
exit 0
