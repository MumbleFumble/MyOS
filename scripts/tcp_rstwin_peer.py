#!/usr/bin/env python3
"""Host-side peer for test_rst_window.sh (Phase 13 inc.4,
review-009).

Proves both directions of the RST sequence validation against a
live tcpechosrv connection:
  1. handshake, send P1, read the echo — its ACK field is the
     guest's rcv_nxt (R).
  2. send two spoofed RSTs (seq = R + 100000 and seq = R - 1,
     both outside the receive window), then send P2 — the echo
     of P2 proves the connection survived.
  3. send a legitimate RST (seq = the guest's current rcv_nxt)
     — the connection must die: a further payload P3 gets no
     echo (the guest answers with at most a bare RST).
Writes PEER OK / PEER FAIL to the result file (argv[1]).
"""
import socket
import struct
import sys
import time

RESULT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/tcp_rstwin_peer_result.txt"
GUEST_IP = bytes([192, 168, 77, 2])
PEER_IP = bytes([192, 168, 77, 1])
GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000099")
SPORT = 40001
DPORT = 8080
ISS = 2000
P1 = b"window-probe-1"
P2 = b"still-alive-2"
P3 = b"should-not-echo"
DEADLINE = time.time() + 170

F_FIN, F_SYN, F_RST, F_PSH, F_ACK = 0x01, 0x02, 0x04, 0x08, 0x10


def done(msg):
    with open(RESULT, "w") as f:
        f.write(msg + "\n")
    sys.exit(0 if msg.startswith("PEER OK") else 1)


def cksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack(">%dH" % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


s = None
while time.time() < DEADLINE:
    try:
        s = socket.create_connection(("127.0.0.1", 10000), timeout=5)
        break
    except OSError:
        time.sleep(1)
if s is None:
    done("PEER FAIL: could not connect to QEMU socket")
s.settimeout(0.3)

rxbuf = b""


def poll_frames():
    global rxbuf
    frames = []
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            rxbuf += chunk
            while len(rxbuf) >= 4:
                n = struct.unpack(">I", rxbuf[:4])[0]
                if len(rxbuf) < 4 + n:
                    break
                frames.append(rxbuf[4:4 + n])
                rxbuf = rxbuf[4 + n:]
            if len(chunk) < 65536:
                break
    except (socket.timeout, BlockingIOError):
        pass
    return frames


def send_frame(fr):
    s.sendall(struct.pack(">I", len(fr)) + fr)


def send_tcp(seq, ack, flags, data=b""):
    seg = struct.pack(">HHII", SPORT, DPORT, seq, ack) \
        + struct.pack(">BBHHH", 5 << 4, flags, 2048, 0, 0) + data
    pseudo = PEER_IP + GUEST_IP + struct.pack(">BBH", 0, 6, len(seg))
    seg = seg[:16] + struct.pack(">H", cksum(pseudo + seg)) + seg[18:]
    total = 20 + len(seg)
    ip = struct.pack(">BBHHH", 0x45, 0, total, 0x5555, 0) \
        + struct.pack(">BB", 64, 6) + b"\x00\x00" + PEER_IP + GUEST_IP
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    send_frame((GUEST_MAC + PEER_MAC + b"\x08\x00" + ip + seg).ljust(60, b"\x00"))


def parse(fr):
    """(seq, ack, flags, payload) for a guest segment to us."""
    if len(fr) < 14:
        return None
    if fr[12:14] == b"\x08\x06" and len(fr) >= 42:
        arp = fr[14:42]
        if arp[6:8] == b"\x00\x01" and arp[24:28] == PEER_IP:
            reply = struct.pack(">HHBBH", 1, 0x0800, 6, 4, 2) \
                + PEER_MAC + PEER_IP + arp[8:14] + arp[14:18]
            send_frame((arp[8:14] + PEER_MAC + b"\x08\x06" + reply).ljust(60, b"\x00"))
        return None
    if fr[12:14] != b"\x08\x00" or len(fr) < 14 + 20 + 20:
        return None
    pkt = fr[14:]
    ihl = (pkt[0] & 0x0F) * 4
    tot = struct.unpack(">H", pkt[2:4])[0]
    if pkt[9] != 6 or pkt[16:20] != PEER_IP:
        return None
    seg = pkt[ihl:tot]
    sport, dport, seq, ack = struct.unpack(">HHII", seg[:12])
    if sport != DPORT or dport != SPORT:
        return None
    doff = (seg[12] >> 4) * 4
    return seq, ack, seg[13], seg[doff:]


phase = "syn"          # syn -> probe1 -> spoof -> probe2 -> verify
last_syn = 0.0
giss = None
gseq = None            # next guest seq we expect
snd_nxt = ISS + 1
rcv_nxt = None         # guest's rcv_nxt, read off its ACKs
phase_at = time.time()
p3_sent = False

while time.time() < DEADLINE:
    now = time.time()
    if phase == "syn" and now - last_syn >= 1.0:
        send_tcp(ISS, 0, F_SYN)
        last_syn = now
    elif phase == "spoof" and now - phase_at >= 1.5:
        send_tcp(rcv_nxt, gseq, F_PSH | F_ACK, P2)
        snd_nxt = rcv_nxt + len(P2)
        phase, phase_at = "probe2", now
    elif phase == "probe2" and now - phase_at > 12:
        done("PEER FAIL: no echo after spoofed RSTs (connection killed?)")
    elif phase == "verify":
        if not p3_sent and now - phase_at >= 1.0:
            send_tcp(snd_nxt, gseq, F_PSH | F_ACK, P3)
            p3_sent = True
        elif now - phase_at > 8:
            done("PEER OK")

    for fr in poll_frames():
        r = parse(fr)
        if r is None:
            continue
        seq, ack, flags, payload = r
        if phase == "syn":
            if (flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) and ack == ISS + 1:
                giss = seq
                gseq = seq + 1
                send_tcp(snd_nxt, gseq, F_ACK)
                send_tcp(snd_nxt, gseq, F_PSH | F_ACK, P1)
                snd_nxt += len(P1)
                phase, phase_at = "probe1", time.time()
            continue
        if payload:
            if seq == gseq:
                gseq += len(payload)
                send_tcp(snd_nxt, gseq, F_ACK)
                if phase == "probe1":
                    if payload != P1:
                        done(f"PEER FAIL: echo 1 mismatch: {payload!r}")
                    if ack != ISS + 1 + len(P1):
                        done(f"PEER FAIL: echo 1 ack {ack}, expected {ISS + 1 + len(P1)}")
                    rcv_nxt = ack
                    # Two spoofed resets, both out of window.
                    send_tcp((rcv_nxt + 100000) & 0xFFFFFFFF, gseq, F_RST)
                    send_tcp((rcv_nxt - 1) & 0xFFFFFFFF, gseq, F_RST)
                    phase, phase_at = "spoof", time.time()
                elif phase == "probe2":
                    if payload != P2:
                        done(f"PEER FAIL: echo 2 mismatch: {payload!r}")
                    if ack != rcv_nxt + len(P2):
                        done(f"PEER FAIL: echo 2 ack {ack}, expected {rcv_nxt + len(P2)}")
                    # Legitimate in-window reset: seq == rcv_nxt.
                    send_tcp(ack, gseq, F_RST)
                    snd_nxt = ack
                    phase, phase_at = "verify", time.time()
                elif phase == "verify":
                    done(f"PEER FAIL: data echoed after in-window RST: {payload!r}")
            else:
                send_tcp(snd_nxt, gseq, F_ACK)   # dup-ack for a retransmit
    time.sleep(0.1)

done(f"PEER FAIL: timeout in phase {phase}")
