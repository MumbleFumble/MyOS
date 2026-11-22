#!/usr/bin/env python3
"""Host-side TCP client for test_tcpechosrv.sh (Phase 12 inc.3).

Connects to the guest's echo server (192.168.77.2:8080) from
192.168.77.1:40001 over QEMU's TCP-stream socket netdev, retries
the SYN until the guest listens, completes the handshake, sends a
payload, and asserts the echoed bytes come back byte-for-byte
(with correct seq/ack and checksums). Then it FINs; the guest
must FIN back after echoing, and the peer ACKs. Writes PEER OK /
PEER FAIL to the result file (argv[1]).
"""
import socket
import struct
import sys
import time

RESULT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/tcp_echo_peer_result.txt"
GUEST_IP = bytes([192, 168, 77, 2])
PEER_IP = bytes([192, 168, 77, 1])
GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000099")
SPORT = 40001
DPORT = 8080
ISS = 2000
PAYLOAD = b"echo-round-trip-42"
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


def send_tcp(seq, ack, flags, payload=b""):
    seg = struct.pack(">HHII", SPORT, DPORT, seq, ack) \
        + struct.pack(">BBHHH", 5 << 4, flags, 2048, 0, 0) + payload
    pseudo = PEER_IP + GUEST_IP + struct.pack(">BBH", 0, 6, len(seg))
    seg = seg[:16] + struct.pack(">H", cksum(pseudo + seg)) + seg[18:]
    total = 20 + len(seg)
    ip = struct.pack(">BBHHH", 0x45, 0, total, 0x5555, 0) \
        + struct.pack(">BB", 64, 6) + b"\x00\x00" + PEER_IP + GUEST_IP
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    send_frame((GUEST_MAC + PEER_MAC + b"\x08\x00" + ip + seg).ljust(60, b"\x00"))


def parse(fr):
    """(seq, ack, flags, payload) for a guest segment to us, with
    both checksums verified; None for anything else."""
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
    if cksum(pkt[:ihl]) != 0:
        done("PEER FAIL: guest IP checksum invalid")
    seg = pkt[ihl:tot]
    pseudo = GUEST_IP + PEER_IP + struct.pack(">BBH", 0, 6, len(seg))
    if cksum(pseudo + seg) != 0:
        done("PEER FAIL: guest TCP checksum invalid")
    sport, dport, seq, ack = struct.unpack(">HHII", seg[:12])
    if sport != DPORT or dport != SPORT:
        return None
    doff = (seg[12] >> 4) * 4
    return seq, ack, seg[13], seg[doff:]


state = "syn"
giss = None
echoed = b""
last_syn = 0.0
while time.time() < DEADLINE:
    if state == "syn" and time.time() - last_syn >= 1.0:
        send_tcp(ISS, 0, F_SYN)
        last_syn = time.time()
    for fr in poll_frames():
        r = parse(fr)
        if r is None:
            continue
        seq, ack, flags, payload = r
        if state == "syn":
            if flags & F_RST:
                continue
            if (flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK):
                if ack != ISS + 1:
                    done(f"PEER FAIL: SYN-ACK ack {ack} != {ISS + 1}")
                giss = seq
                send_tcp(ISS + 1, giss + 1, F_ACK)
                send_tcp(ISS + 1, giss + 1, F_PSH | F_ACK, PAYLOAD)
                state = "echo"
        elif state == "echo":
            if payload:
                if seq != giss + 1 + len(echoed):
                    done(f"PEER FAIL: echo seq {seq}, expected {giss + 1 + len(echoed)}")
                echoed += payload
                if ack != ISS + 1 + len(PAYLOAD):
                    done(f"PEER FAIL: echo ack {ack} != {ISS + 1 + len(PAYLOAD)}")
                if len(echoed) == len(PAYLOAD):
                    if echoed != PAYLOAD:
                        done(f"PEER FAIL: echo {echoed!r} != {PAYLOAD!r}")
                    send_tcp(ISS + 1 + len(PAYLOAD), giss + 1 + len(PAYLOAD),
                             F_FIN | F_ACK)
                    state = "fin"
                elif len(echoed) > len(PAYLOAD):
                    done("PEER FAIL: guest echoed more than was sent")
        elif state == "fin":
            if flags & F_FIN:
                send_tcp(ISS + 2 + len(PAYLOAD), seq + 1, F_ACK)
                done("PEER OK")
    time.sleep(0.05)

done(f"PEER FAIL: timeout in state {state} (echoed {len(echoed)}/{len(PAYLOAD)} bytes)")
