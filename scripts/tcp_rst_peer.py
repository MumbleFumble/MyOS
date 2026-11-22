#!/usr/bin/env python3
"""Host-side peer for test_tcprst.sh (Phase 12 inc.4, review-001).

Phase FILL: SYNs from ports 40011-40013 claim every non-listener
slot as SYN_RCVD and are never ACKed. The guest's accept times
out; without the inc.4 cleanup the listener and the half-open
slots leak and the table stays full.
Phase A: a real connection from 40001 must then succeed (proves
the timeout path freed the slots and a re-listen works).
Phase B: a connection from 40002 is accepted, then killed with
an RST instead of a FIN swap (proves the RST path frees the
accepted slot and the close path still completes).
Phase C: another real connection from 40001 must succeed.
Writes PEER OK / PEER FAIL to the result file (argv[1]).
"""
import socket
import struct
import sys
import time

RESULT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/tcp_rst_peer_result.txt"
GUEST_IP = bytes([192, 168, 77, 2])
PEER_IP = bytes([192, 168, 77, 1])
GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000099")
DPORT = 8080
FILL_PORTS = [40011, 40012, 40013]
FILL_ISS = 3000
A_PORT, A_ISS = 40001, 2000
B_PORT, B_ISS = 40002, 5000
C_PORT, C_ISS = 40001, 7000
START = time.time()
DEADLINE = START + 280

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


def send_tcp(sport, seq, ack, flags):
    seg = struct.pack(">HHII", sport, DPORT, seq, ack) \
        + struct.pack(">BBHHH", 5 << 4, flags, 2048, 0, 0)
    pseudo = PEER_IP + GUEST_IP + struct.pack(">BBH", 0, 6, len(seg))
    seg = seg[:16] + struct.pack(">H", cksum(pseudo + seg)) + seg[18:]
    total = 20 + len(seg)
    ip = struct.pack(">BBHHH", 0x45, 0, total, 0x5555, 0) \
        + struct.pack(">BB", 64, 6) + b"\x00\x00" + PEER_IP + GUEST_IP
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    send_frame((GUEST_MAC + PEER_MAC + b"\x08\x00" + ip + seg).ljust(60, b"\x00"))


def parse(fr):
    """(our_port, seq, ack, flags) for a guest segment to us."""
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
    if sport != DPORT:
        return None
    return dport, seq, ack, seg[13]


class Conn:
    """One real connection's handshake + FIN-swap state machine."""

    def __init__(self, port, iss, phase):
        self.port, self.iss, self.phase = port, iss, phase
        self.state = "syn"
        self.giss = None
        self.last_syn = 0.0

    def drive(self):
        if self.state == "syn" and time.time() - self.last_syn >= 1.0:
            send_tcp(self.port, self.iss, 0, F_SYN)
            self.last_syn = time.time()

    def on_seg(self, seq, ack, flags):
        """Return True when this connection's part is complete."""
        if self.state == "syn":
            if flags & F_RST:
                return False
            if (flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) and ack == self.iss + 1:
                self.giss = seq
                send_tcp(self.port, self.iss + 1, self.giss + 1, F_ACK)
                # "rst" phase: wait for the guest's FIN first (it
                # proves the accept syscall returned), then kill
                # the connection with an RST instead of ACKing.
                self.state = "waitfin" if self.phase == "rst" else "fin"
            return False
        if self.state == "waitfin":
            if flags & F_FIN:
                send_tcp(self.port, self.iss + 1, 0, F_RST)
                return True
            return False
        if self.state == "fin":
            if flags & F_FIN:
                send_tcp(self.port, self.iss + 1, seq + 1, F_ACK)
                send_tcp(self.port, self.iss + 1, seq + 1, F_FIN | F_ACK)
                self.state = "finack"
            return False
        if self.state == "finack":
            if flags & F_FIN:
                send_tcp(self.port, self.iss + 2, seq + 1, F_ACK)
            elif (flags & F_ACK) and ack == self.iss + 2:
                return True
            return False
        return False


conns = {}          # our port -> Conn (active real connection)
phase = "fill"      # fill -> A -> B -> C
cur = None
while time.time() < DEADLINE:
    now = time.time()
    if phase == "fill":
        if now - START < 48:
            for p in FILL_PORTS:
                send_tcp(p, FILL_ISS, 0, F_SYN)
        else:
            phase = "A"
            cur = Conn(A_PORT, A_ISS, "swap")
    elif phase in ("A", "B", "C") and cur is not None:
        cur.drive()
    for fr in poll_frames():
        r = parse(fr)
        if r is None:
            continue
        dport, seq, ack, flags = r
        if dport in FILL_PORTS:
            continue  # SYN-ACKs for half-open fills: never ACK
        if cur is not None and dport == cur.port:
            if cur.on_seg(seq, ack, flags):
                if phase == "A":
                    phase, cur = "B", Conn(B_PORT, B_ISS, "rst")
                elif phase == "B":
                    phase, cur = "C", Conn(C_PORT, C_ISS, "swap")
                elif phase == "C":
                    done("PEER OK")
    time.sleep(0.2)

done(f"PEER FAIL: timeout in phase {phase}")
