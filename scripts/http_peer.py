#!/usr/bin/env python3
"""Host-side HTTP client for test_httpd.sh (Phase 12 inc.5).

Two sequential connections to the guest httpd (192.168.77.2:8080)
over QEMU's TCP-stream socket netdev:
  1. from :40001, GET /hello.txt — the full response must equal
     "HTTP/1.0 200 OK\r\nContent-Length: 11\r\n\r\nhello-myfs\n"
     (the file the test wrote via the shell `write` command,
     which appends a newline).
  2. from :40002, GET /nosuch.txt — the full response must equal
     the guest's 404 page.
Each connection ends with the guest's FIN; the peer ACKs it,
sends its own FIN, and expects the final ACK. Checksums and
sequence numbers are validated throughout. Writes PEER OK /
PEER FAIL to the result file (argv[1]).
"""
import socket
import struct
import sys
import time

RESULT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/http_peer_result.txt"
GUEST_IP = bytes([192, 168, 77, 2])
PEER_IP = bytes([192, 168, 77, 1])
GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000099")
DPORT = 8080
EXPECT1 = b"HTTP/1.0 200 OK\r\nContent-Length: 11\r\n\r\nhello-myfs\n"
EXPECT2 = b"HTTP/1.0 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found"
DEADLINE = time.time() + 240

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


def send_tcp(sport, seq, ack, flags, payload=b""):
    seg = struct.pack(">HHII", sport, DPORT, seq, ack) \
        + struct.pack(">BBHHH", 5 << 4, flags, 2048, 0, 0) + payload
    pseudo = PEER_IP + GUEST_IP + struct.pack(">BBH", 0, 6, len(seg))
    seg = seg[:16] + struct.pack(">H", cksum(pseudo + seg)) + seg[18:]
    total = 20 + len(seg)
    ip = struct.pack(">BBHHH", 0x45, 0, total, 0x5555, 0) \
        + struct.pack(">BB", 64, 6) + b"\x00\x00" + PEER_IP + GUEST_IP
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    send_frame((GUEST_MAC + PEER_MAC + b"\x08\x00" + ip + seg).ljust(60, b"\x00"))


def parse(fr):
    """(our_port, seq, ack, flags, payload) for a guest segment."""
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
    if sport != DPORT:
        return None
    doff = (seg[12] >> 4) * 4
    return dport, seq, ack, seg[13], seg[doff:]


class HttpConn:
    def __init__(self, port, iss, request, expect, label):
        self.port, self.iss = port, iss
        self.request, self.expect, self.label = request, expect, label
        self.state = "syn"
        self.giss = None
        self.data = b""
        self.next_seq = None
        self.last_syn = 0.0

    def drive(self):
        if self.state == "syn" and time.time() - self.last_syn >= 1.0:
            send_tcp(self.port, self.iss, 0, F_SYN)
            self.last_syn = time.time()

    def on_seg(self, seq, ack, flags, payload):
        if self.state == "syn":
            if flags & F_RST:
                return False
            if (flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) and ack == self.iss + 1:
                self.giss = seq
                self.next_seq = seq + 1
                send_tcp(self.port, self.iss + 1, self.giss + 1, F_ACK)
                send_tcp(self.port, self.iss + 1, self.giss + 1,
                         F_PSH | F_ACK, self.request)
                self.state = "data"
            return False
        if self.state == "data":
            if payload and seq == self.next_seq:
                self.data += payload
                self.next_seq += len(payload)
                send_tcp(self.port, self.iss + 1 + len(self.request),
                         self.next_seq, F_ACK)
            elif payload and seq < self.next_seq:
                # Retransmit of bytes already held: re-ACK, ignore.
                send_tcp(self.port, self.iss + 1 + len(self.request),
                         self.next_seq, F_ACK)
            if flags & F_FIN:
                if self.data != self.expect:
                    done(f"PEER FAIL: {self.label} response {self.data!r} != {self.expect!r}")
                send_tcp(self.port, self.iss + 1 + len(self.request),
                         self.next_seq + 1, F_ACK)
                send_tcp(self.port, self.iss + 1 + len(self.request),
                         self.next_seq + 1, F_FIN | F_ACK)
                self.state = "finack"
            return False
        if self.state == "finack":
            if flags & F_FIN:
                send_tcp(self.port, self.iss + 2 + len(self.request),
                         self.next_seq + 1, F_ACK)
            elif (flags & F_ACK) and ack == self.iss + 2 + len(self.request):
                return True
            return False
        return False


conns = [
    HttpConn(40001, 2000, b"GET /hello.txt HTTP/1.0\r\nHost: x\r\n\r\n",
             EXPECT1, "200"),
    HttpConn(40002, 5000, b"GET /nosuch.txt HTTP/1.0\r\n\r\n",
             EXPECT2, "404"),
]
idx = 0
while time.time() < DEADLINE:
    cur = conns[idx]
    cur.drive()
    for fr in poll_frames():
        r = parse(fr)
        if r is None:
            continue
        dport, seq, ack, flags, payload = r
        if dport != cur.port:
            continue
        if cur.on_seg(seq, ack, flags, payload):
            idx += 1
            if idx == len(conns):
                done("PEER OK")
    time.sleep(0.05)

done(f"PEER FAIL: timeout on connection {idx + 1} (state {conns[idx].state}, got {len(conns[idx].data)} bytes)")
