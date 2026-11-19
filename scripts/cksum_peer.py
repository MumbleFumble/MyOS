#!/usr/bin/env python3
"""Host-side peer for test_cksum.sh (Phase 11 inc.1).

Verifies the guest drops packets with bad checksums and still
answers good ones, over QEMU's TCP-stream socket netdev:
  1. good ICMP echo            -> reply (liveness)
  2. bad ICMP checksum echo    -> silence
  3. bad IP header checksum    -> silence
  4. good ICMP echo            -> reply (still alive)
  5. bad UDP checksum echo     -> silence
  6. good UDP echo             -> reply
  7. bad TCP checksum SYN      -> silence (not even an RST)
  8. good TCP SYN              -> RST+ACK (CLOSED-state behavior)
Writes PEER OK / PEER FAIL: <reason> to the result file.
"""
import socket
import struct
import sys
import time

RESULT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/cksum_peer_result.txt"
GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000099")
GUEST_IP = bytes([192, 168, 77, 2])
PEER_IP = bytes([192, 168, 77, 1])
DEADLINE = time.time() + 150


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


def corrupt(v):
    c = v ^ 0x00FF
    return c if c != 0 else 0x1234


def ip_packet(proto, payload, good_ip=True):
    total = 20 + len(payload)
    hdr = struct.pack(">BBHHHBBH", 0x45, 0, total, 1, 0, 64, proto, 0)
    hdr += PEER_IP + GUEST_IP
    c = cksum(hdr)
    if not good_ip:
        c = corrupt(c)
    hdr = hdr[:10] + struct.pack(">H", c) + hdr[12:]
    return hdr + payload


def eth(ip_payload):
    return GUEST_MAC + PEER_MAC + struct.pack(">H", 0x0800) + ip_payload


def icmp_echo(good=True, seq=3):
    msg = struct.pack(">BBHHH", 8, 0, 0, 0x4242, seq) + b"CKSUMPING"
    c = cksum(msg)
    if not good:
        c = corrupt(c)
    msg = msg[:2] + struct.pack(">H", c) + msg[4:]
    return eth(ip_packet(1, msg))


def icmp_bad_ip(seq=4):
    msg = struct.pack(">BBHHH", 8, 0, 0, 0x4242, seq) + b"CKSUMPING"
    msg = msg[:2] + struct.pack(">H", cksum(msg)) + msg[4:]
    return eth(ip_packet(1, msg, good_ip=False))


def udp_echo(good=True, tag=b"A"):
    payload = b"CKSUM-UDP" + tag
    ulen = 8 + len(payload)
    seg = struct.pack(">HHHH", 40000, 7, ulen, 0) + payload
    pseudo = PEER_IP + GUEST_IP + struct.pack(">BBH", 0, 17, ulen)
    c = cksum(pseudo + seg)
    if c == 0:
        c = 0xFFFF
    if not good:
        c = corrupt(c)
    seg = seg[:6] + struct.pack(">H", c) + seg[8:]
    return eth(ip_packet(17, seg))


def tcp_syn(good=True, seq=0x1111):
    seg = struct.pack(">HHIIHHHH", 40001, 80, seq, 0, 5 << 12, 4096, 0, 0)
    seg = seg[:13] + bytes([2]) + seg[14:]  # flags = SYN
    pseudo = PEER_IP + GUEST_IP + struct.pack(">BBH", 0, 6, len(seg))
    c = cksum(pseudo + seg)
    if not good:
        c = corrupt(c)
    seg = seg[:16] + struct.pack(">H", c) + seg[18:]
    return eth(ip_packet(6, seg))


s = None
while time.time() < DEADLINE:
    try:
        s = socket.create_connection(("127.0.0.1", 10000), timeout=5)
        break
    except OSError:
        time.sleep(1)
if s is None:
    done("PEER FAIL: could not connect to QEMU socket")
s.settimeout(0.5)

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
    except socket.timeout:
        pass
    except BlockingIOError:
        pass
    return frames


def send(frame):
    s.sendall(struct.pack(">I", len(frame)) + frame)


def collect(seconds):
    out = []
    end = time.time() + seconds
    while time.time() < end:
        out += poll_frames()
        time.sleep(0.1)
    return out


def is_icmp_reply(f, seq=None):
    if not (len(f) > 14 + 20 and f[12:14] == b"\x08\x00"
            and f[14 + 9] == 1 and f[14 + 20] == 0):
        return False
    if seq is not None:
        return struct.unpack(">H", f[14 + 20 + 6:14 + 20 + 8])[0] == seq
    return True


def is_udp_reply(f, tag=b"A"):
    if len(f) <= 14 + 20 + 8 or f[12:14] != b"\x08\x00" or f[14 + 9] != 17:
        return False
    udp = f[14 + 20:]
    return struct.unpack(">H", udp[0:2])[0] == 7 and b"CKSUM-UDP" + tag in udp[8:]


def is_tcp_rst(f, seq=None):
    if len(f) <= 14 + 20 + 20 or f[12:14] != b"\x08\x00" or f[14 + 9] != 6:
        return False
    if not (f[14 + 20 + 13] & 0x04):
        return False
    if seq is not None:
        return struct.unpack(">I", f[14 + 20 + 8:14 + 20 + 12])[0] == seq + 1
    return True


def expect_reply(frame, pred, what, retries=6):
    for _ in range(retries):
        send(frame)
        for f in collect(3):
            if pred(f):
                return
    done("PEER FAIL: no reply to " + what)


def expect_silence(frame, pred, what):
    send(frame)
    for f in collect(2.5):
        if pred(f):
            done("PEER FAIL: guest answered " + what + " despite bad checksum")


expect_reply(icmp_echo(True, 3), lambda f: is_icmp_reply(f, 3), "good icmp echo")
collect(1)
expect_silence(icmp_echo(False, 4), lambda f: is_icmp_reply(f, 4), "bad-icmp-checksum echo")
expect_silence(icmp_bad_ip(5), lambda f: is_icmp_reply(f, 5), "bad-ip-checksum echo")
expect_reply(icmp_echo(True, 6), lambda f: is_icmp_reply(f, 6), "good icmp echo after drops")
collect(1)
expect_silence(udp_echo(False, b"B"), lambda f: is_udp_reply(f, b"B"), "bad-udp-checksum echo")
expect_reply(udp_echo(True, b"C"), lambda f: is_udp_reply(f, b"C"), "good udp echo")
collect(1)
expect_silence(tcp_syn(False, 0x1111), lambda f: is_tcp_rst(f, 0x1111), "bad-tcp-checksum SYN")
expect_reply(tcp_syn(True, 0x2222), lambda f: is_tcp_rst(f, 0x2222), "good tcp SYN (expect RST)")

done("PEER OK")
