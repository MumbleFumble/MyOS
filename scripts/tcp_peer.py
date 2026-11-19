#!/usr/bin/env python3
"""TCP peer for scripts/test_tcpecho.sh (socket backend, TCP stream).

A minimal TCP server on port 8080 over raw frames. Verifies the
guest's checksums on every segment, the SYN, the exact data payload,
and that the guest ACKs our FIN and sends its own. Sends a greeting
on connect, echoes the guest's data, then closes. Prints PEER OK /
PEER FAIL: <reason>.
"""
import socket
import struct
import sys
import time

GUEST_IP = bytes([192, 168, 77, 2])
PEER_IP = bytes([192, 168, 77, 1])
PEER_MAC = bytes.fromhex("020000000099")
PORT = 8080
ISS = 1000
GREETING = b"HELLO-TCP "
EXPECT = b"hello-myos"
DEADLINE = time.time() + 150

F_FIN, F_SYN, F_RST, F_PSH, F_ACK = 0x01, 0x02, 0x04, 0x08, 0x10


def cksum(data):
    if len(data) % 2:
        data += b"\x00"
    s = sum(struct.unpack(">%dH" % (len(data) // 2), data))
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return (~s) & 0xFFFF


def fail(msg):
    print(f"PEER FAIL: {msg}", flush=True)
    sys.exit(1)


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 10000))
srv.listen(1)
conn, _ = srv.accept()
conn.settimeout(5)
buf = b""


def poll_frames():
    global buf
    try:
        data = conn.recv(65536)
        if data:
            buf += data
    except (socket.timeout, TimeoutError):
        pass
    frames = []
    while len(buf) >= 4:
        (ln,) = struct.unpack(">I", buf[:4])
        if len(buf) < 4 + ln:
            break
        frames.append(buf[4:4 + ln])
        buf = buf[4 + ln:]
    return frames


def send_frame(fr):
    conn.sendall(struct.pack(">I", len(fr)) + fr)


gport = None          # guest ephemeral port
gseq0 = None          # guest ISS
nxt = ISS             # our next seq
fin_seq = None
state = "listen"
got_data = False
guest_fin = False
acked_fin = False


def send_tcp(seq, ack, flags, payload=b""):
    global nxt
    seg = struct.pack(">HHII", PORT, gport, seq, ack) \
        + struct.pack(">BBHHH", 5 << 4, flags, 2048, 0, 0) + payload
    pseudo = PEER_IP + GUEST_IP + struct.pack(">BBH", 0, 6, len(seg))
    seg = seg[:16] + struct.pack(">H", cksum(pseudo + seg)) + seg[18:]
    total = 20 + len(seg)
    ip = struct.pack(">BBHHH", 0x45, 0, total, 0x5555, 0) \
        + struct.pack(">BB", 64, 6) + b"\x00\x00" + PEER_IP + GUEST_IP
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    fr = (bytes.fromhex("525400123456") + PEER_MAC + b"\x08\x00" + ip + seg)
    send_frame(fr.ljust(60, b"\x00"))
    nxt = seq + len(payload) + (1 if flags & (F_SYN | F_FIN) else 0)


def handle(fr):
    global gport, gseq0, state, got_data, guest_fin, acked_fin, fin_seq
    if len(fr) < 14:
        return
    etype = fr[12:14]
    if etype == b"\x08\x06" and len(fr) >= 42:
        arp = fr[14:42]
        if arp[6:8] == b"\x00\x01" and arp[24:28] == PEER_IP:
            reply = struct.pack(">HHBBH", 1, 0x0800, 6, 4, 2) \
                + PEER_MAC + PEER_IP + arp[8:14] + arp[14:18]
            send_frame((arp[8:14] + PEER_MAC + b"\x08\x06" + reply).ljust(60, b"\x00"))
        return
    if etype != b"\x08\x00" or len(fr) < 14 + 20 + 20:
        return
    pkt = fr[14:]
    ihl = (pkt[0] & 0x0F) * 4
    tot = struct.unpack(">H", pkt[2:4])[0]
    if pkt[9] != 6 or pkt[16:20] != PEER_IP:
        return
    seg = pkt[ihl:tot]
    sport, dport, seq, ack = struct.unpack(">HHII", seg[:12])
    doff = (seg[12] >> 4) * 4
    flags = seg[13]
    payload = seg[doff:]
    if dport != PORT:
        return
    # Guest checksum must verify (sum over pseudo-header + segment == 0).
    pseudo = GUEST_IP + PEER_IP + struct.pack(">BBH", 0, 6, len(seg))
    if cksum(pseudo + seg) != 0:
        fail("guest TCP checksum invalid")
    if state == "listen":
        if flags != F_SYN:
            fail(f"expected bare SYN, got flags {flags:#x}")
        gport, gseq0 = sport, seq
        send_tcp(ISS, seq + 1, F_SYN | F_ACK)
        state = "syn"
        return
    if sport != gport:
        return
    if state == "syn":
        if (flags & F_ACK) and ack == ISS + 1:
            state = "est"
            send_tcp(nxt, seq, F_PSH | F_ACK, GREETING)
        return
    # established
    if payload:
        if payload != EXPECT:
            fail(f"data payload {payload!r} != {EXPECT!r}")
        got_data = True
        send_tcp(nxt, seq + len(payload), F_PSH | F_ACK, payload)  # echo
        fin_seq = nxt
        send_tcp(nxt, seq + len(payload), F_FIN | F_ACK)
        state = "closing"
        return
    if flags & F_FIN:
        guest_fin = True
        send_tcp(nxt, seq + 1, F_ACK)
    if fin_seq is not None and (flags & F_ACK) and ack == fin_seq + 1:
        acked_fin = True
    if got_data and guest_fin and acked_fin:
        print("PEER OK", flush=True)
        sys.exit(0)


while time.time() < DEADLINE:
    for fr in poll_frames():
        handle(fr)
    time.sleep(0.02)
fail(f"timeout: state={state} got_data={got_data} guest_fin={guest_fin} acked_fin={acked_fin}")
