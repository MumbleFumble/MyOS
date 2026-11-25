#!/usr/bin/env bash
# Headless test for Phase 15 inc.4: the advertised receive
# window is the connection's real free space, a window update
# is emitted when the application reads, and a zero window
# does not deadlock (the update reopens the flow, and an
# old-sequence zero-length probe is answered with the current
# window in case the update was the segment that got lost).
#
# The guest runs user/wintest.c (a server that reads on an
# observable schedule); the peer below is a TCP client that
# watches the window field on every guest segment:
#   SYN-ACK win=2048 -> burst A (1024) -> guest ACK win~1024
#   -> burst B (1025) -> guest ACK win=0 -> silence ->
#   window update win=512 (dt >= 0.35s after the zero) ->
#   probe (seq = snd_una - 1, len 0) -> ACK win=512 ->
#   burst C (512) -> guest verifies 2561 pattern bytes and
#   replies WIN-OK -> clean FIN swap.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT/build/myos.iso"
DISK="$ROOT/build/disk.img"
SERIAL=/tmp/myos_window_serial.txt
SOCK=/tmp/qemu_window_mon.sock
RESULT=/tmp/window_peer_result.txt
PEERLOG=/tmp/myos_window_peer.log
PEERPY=/tmp/myos_window_peer.py

[ -f "$ISO" ] || { echo "[test] FAIL: $ISO missing (run make iso first)"; exit 1; }

cat > "$PEERPY" << 'PYEOF'
import socket, struct, sys, time

RESULT = sys.argv[1]
LOGP = sys.argv[2]
GUEST_IP = bytes([192, 168, 77, 2]); PEER_IP = bytes([192, 168, 77, 1])
GUEST_MAC = bytes.fromhex("525400123456"); PEER_MAC = bytes.fromhex("020000000099")
SPORT, DPORT, ISS = 40001, 8080, 2000
DEADLINE = time.time() + 240
F_FIN, F_SYN, F_RST, F_PSH, F_ACK = 0x01, 0x02, 0x04, 0x08, 0x10
logf = open(LOGP, "w", buffering=1)
t0 = time.time()
def log(m): logf.write(f"[{time.time()-t0:8.3f}] {m}\n")
def done(msg):
    log(msg)
    with open(RESULT, "w") as f: f.write(msg + "\n")
    sys.exit(0 if msg.startswith("PEER OK") else 1)
def cksum(d):
    if len(d) % 2: d += b"\x00"
    t = sum(struct.unpack(">%dH" % (len(d)//2), d))
    while t >> 16: t = (t & 0xFFFF) + (t >> 16)
    return (~t) & 0xFFFF

s = None
while time.time() < DEADLINE:
    try:
        s = socket.create_connection(("127.0.0.1", 10000), timeout=5); break
    except OSError: time.sleep(1)
if s is None: done("PEER FAIL: could not connect to QEMU socket")
s.settimeout(0.2)
rxbuf = b""
def poll():
    global rxbuf
    try:
        d = s.recv(65536)
        if d: rxbuf += d
    except (socket.timeout, BlockingIOError): pass
    out = []
    while len(rxbuf) >= 4:
        (n,) = struct.unpack(">I", rxbuf[:4])
        if len(rxbuf) < 4 + n: break
        out.append(rxbuf[4:4+n]); rxbuf = rxbuf[4+n:]
    return out
def parse(f):
    if len(f) < 54 or f[12:14] != b"\x08\x00": return None
    ip = f[14:]
    if ip[9] != 6 or ip[12:16] != GUEST_IP: return None
    t = ip[20:]
    sp, dp = struct.unpack(">HH", t[:4])
    if sp != DPORT or dp != SPORT: return None
    seq, ack = struct.unpack(">II", t[4:12])
    fl = t[13]; win = struct.unpack(">H", t[14:16])[0]
    ln = struct.unpack(">H", ip[2:4])[0] - 20 - (t[12] >> 4) * 4
    return seq, ack, fl, win, t[20:20+ln]
nxt = ISS
def send_tcp(seq, ack, flags, payload=b""):
    global nxt
    th = struct.pack(">HHII", SPORT, DPORT, seq & 0xFFFFFFFF, ack & 0xFFFFFFFF)
    th += bytes([5 << 4, flags]) + struct.pack(">HHH", 4096, 0, 0)
    th = th[:16] + struct.pack(">H", cksum(PEER_IP + GUEST_IP + struct.pack(">BBH", 0, 6, len(th) + len(payload)) + th + payload)) + th[18:]
    iph = struct.pack(">BBHHH", 0x45, 0, 20 + len(th) + len(payload), 1, 0)
    iph += bytes([64, 6]) + struct.pack(">H", 0) + PEER_IP + GUEST_IP
    iph = iph[:10] + struct.pack(">H", cksum(iph)) + iph[12:]
    fr = GUEST_MAC + PEER_MAC + b"\x08\x00" + iph + th + payload
    s.sendall(struct.pack(">I", len(fr)) + fr)
    if payload or (flags & F_SYN) or (flags & F_FIN):
        nxt = seq + len(payload) + (1 if flags & (F_SYN | F_FIN) else 0)
def pat(o): return (o * 7 + 3) % 256
def burst(off, n):
    send_tcp(ISS + 1 + off, gnext, F_PSH | F_ACK, bytes(pat(off + i) for i in range(n)))
def wait_seg(pred, what, timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        for f in poll():
            p = parse(f)
            if p is None: continue
            log(f"<- seq={p[0]} ack={p[1]} fl={p[2]:02x} win={p[3]} len={len(p[4])}")
            if pred(p): return p
        time.sleep(0.02)
    done(f"PEER FAIL: timeout waiting for {what}")

# Handshake: retry the SYN until the guest is listening.
gnext = 0
while time.time() < DEADLINE:
    send_tcp(ISS, 0, F_SYN)
    got = None
    end = time.time() + 2
    while time.time() < end:
        for f in poll():
            p = parse(f)
            if p and (p[2] & F_SYN) and (p[2] & F_ACK) and p[1] == ISS + 1:
                got = p; break
        if got: break
        time.sleep(0.02)
    if got: break
if not got: done("PEER FAIL: no SYN-ACK")
gseq, _, _, synwin, _ = got
gnext = gseq + 1
log(f"WIN synack win={synwin}")
if synwin != 2048: done(f"PEER FAIL: SYN-ACK window {synwin} != 2048")
send_tcp(ISS + 1, gnext, F_ACK)
DS = ISS + 1  # our data start sequence

burst(0, 1024)
r = wait_seg(lambda p: p[1] == DS + 1024 and (p[2] & F_ACK), "ack of burst A")
log(f"WIN after-a win={r[3]}")

burst(1024, 1025)
r = wait_seg(lambda p: p[1] == DS + 2049 and p[3] == 0, "zero window after burst B")
t_zero = time.time()
log("WIN zero win=0")

r = wait_seg(lambda p: p[1] == DS + 2049 and p[3] == 512, "window update", timeout=60)
dt = time.time() - t_zero
log(f"WIN update win=512 dt={dt:.3f}")
if dt < 0.35: done(f"PEER FAIL: update arrived {dt:.3f}s after zero window (not the read)")

send_tcp(DS - 1, gnext, F_ACK)   # zero-length probe, old sequence
r = wait_seg(lambda p: p[1] == DS + 2049 and p[3] == 512, "probe ack", timeout=30)
log("WIN probe-ack win=512")

burst(2049, 512)
reply = b""
fin = [False]
def collect(p):
    global gnext
    seq, ack, fl, win, pl = p
    if pl:
        gnext = seq + len(pl)
        send_tcp(nxt, gnext, F_ACK)
        return pl
    if fl & F_FIN:
        gnext = seq + 1
        send_tcp(nxt, gnext, F_ACK)
        send_tcp(nxt, gnext, F_FIN | F_ACK)
        fin[0] = True
        return b""
    return None
end = time.time() + 90
while time.time() < end:
    for f in poll():
        p = parse(f)
        if p is None: continue
        got = collect(p)
        if got: reply += got
        if fin[0] and reply == b"WIN-OK":
            done("PEER OK")
    time.sleep(0.02)
done(f"PEER FAIL: close incomplete (reply={reply!r} fin={fin[0]})")
PYEOF

for pid in $(pgrep -f "[q]emu-system-x86_64" 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
sleep 1
rm -f "$SERIAL" "$SOCK" "$RESULT" "$PEERLOG"

python3 "$PEERPY" "$RESULT" "$PEERLOG" 2>/tmp/myos_window_peer.err &
PEERPID=$!
qemu-system-x86_64 -m 256 -display none -vga none \
  -serial "file:$SERIAL" -monitor "unix:$SOCK,server,nowait" \
  -boot d -cdrom "$ISO" -drive "file=$DISK,format=raw,if=ide,index=0" \
  -netdev "socket,id=n0,listen=127.0.0.1:10000" \
  -device "e1000,netdev=n0,mac=52:54:00:12:34:56" &
QPID=$!

fail() { echo "[test] FAIL: $1"; [ -f "$PEERLOG" ] && cat "$PEERLOG"; kill -9 "$QPID" "$PEERPID" 2>/dev/null; exit 1; }

for i in $(seq 1 40); do [ -S "$SOCK" ] && break; sleep 0.5; done
sleep 20

type_word() {
  local str="$1" i c
  for (( i=0; i<${#str}; i++ )); do
    c="${str:$i:1}"
    case "$c" in
      ' ') echo "sendkey spc" ;;
      '.') echo "sendkey dot" ;;
      *)   echo "sendkey $c" ;;
    esac
    sleep 0.06
  done
  echo "sendkey ret"
  sleep 0.6
}
{ type_word "exec wintest.elf"; } | socat - UNIX-CONNECT:"$SOCK" >/dev/null 2>&1 || true

for i in $(seq 1 240); do
  grep -q "PEER OK" "$RESULT" 2>/dev/null && break
  grep -q "PEER FAIL" "$RESULT" 2>/dev/null && fail "peer verdict: $(cat "$RESULT")"
  sleep 1
done
grep -q "PEER OK" "$RESULT" 2>/dev/null || fail "peer never finished: $(cat "$RESULT" 2>/dev/null || echo none)"

grep -a -q "wintest: ALL PASS" "$SERIAL" || fail "guest did not print wintest: ALL PASS"
grep -a -q "wintest: FAIL" "$SERIAL" && fail "guest reported a wintest failure"
grep -q "WIN synack win=2048" "$PEERLOG" || fail "no full window on the SYN-ACK"
grep -q "WIN zero win=0" "$PEERLOG" || fail "no zero window observed"
grep -q "WIN update win=512" "$PEERLOG" || fail "no window update observed"
grep -q "WIN probe-ack win=512" "$PEERLOG" || fail "probe was not answered with the reopened window"

echo "[test] PASS: window shrinks with occupancy, hits zero, update on read reopens, probe answered"
kill -9 "$QPID" "$PEERPID" 2>/dev/null
exit 0
