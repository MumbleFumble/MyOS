#!/usr/bin/env python3
"""Host-side DHCP server for test_dhcp_renew.sh (Phase 11 inc.3).

Plays DHCP over QEMU's TCP-stream socket netdev with a 20-second
lease: DISCOVER -> OFFER, REQUEST -> ACK, then the guest must renew
on its own at T1 (lease/2 = 10 s). Two renewals ACKed = PEER OK.
Writes PEER OK / PEER FAIL: <reason> to the result file.
"""
import socket
import struct
import sys
import time

RESULT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/dhcp_peer_result.txt"
GUEST_MAC = bytes.fromhex("525400123456")
PEER_MAC = bytes.fromhex("020000000099")
GUEST_IP = bytes([192, 168, 77, 2])
PEER_IP = bytes([192, 168, 77, 1])
BCAST_MAC = bytes.fromhex("ffffffffffff")
BCAST_IP = bytes([255, 255, 255, 255])
LEASE = 20
DEADLINE = time.time() + 170


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
    except (socket.timeout, BlockingIOError):
        pass
    return frames


def send(frame):
    s.sendall(struct.pack(">I", len(frame)) + frame)


def bootp_reply(req, msg_type):
    p = bytearray(300)
    p[0] = 2  # reply
    p[1] = 1
    p[2] = 6
    p[4:8] = req[4:8]          # xid
    p[16:20] = GUEST_IP        # yiaddr
    p[20:24] = PEER_IP         # siaddr
    p[28:44] = req[28:44]      # chaddr
    p[236:240] = b"\x63\x82\x53\x63"
    opts = struct.pack(">BBB", 53, 1, msg_type)
    opts += struct.pack(">BB", 54, 4) + PEER_IP
    opts += struct.pack(">BB", 1, 4) + bytes([255, 255, 255, 0])
    opts += struct.pack(">BB", 3, 4) + PEER_IP
    opts += struct.pack(">BB", 6, 4) + PEER_IP
    opts += struct.pack(">BBI", 51, 4, LEASE)
    opts += b"\xff"
    p[240:240 + len(opts)] = opts
    return bytes(p)


def dhcp_frame(req_bootp, msg_type):
    body = bootp_reply(req_bootp, msg_type)
    udp = struct.pack(">HHHH", 67, 68, 8 + len(body), 0) + body
    total = 20 + len(udp)
    ip = struct.pack(">BBHHHBBH", 0x45, 0, total, 7, 0, 64, 17, 0)
    ip += PEER_IP + BCAST_IP
    ip = ip[:10] + struct.pack(">H", cksum(ip)) + ip[12:]
    return BCAST_MAC + PEER_MAC + b"\x08\x00" + ip + udp


def msg_type_of(frame):
    """Return (dhcp msg type, bootp payload) or (None, None)."""
    if len(frame) < 14 + 20 + 8 + 240 or frame[12:14] != b"\x08\x00":
        return None, None
    pkt = frame[14:]
    ihl = (pkt[0] & 0x0F) * 4
    if pkt[9] != 17:
        return None, None
    udp = pkt[ihl:]
    sport, dport = struct.unpack(">HH", udp[:4])
    if sport != 68 or dport != 67:
        return None, None
    p = udp[8:]
    if p[236:240] != b"\x63\x82\x53\x63":
        return None, None
    o = 240
    while o + 1 < len(p):
        code = p[o]
        if code == 255:
            break
        if code == 0:
            o += 1
            continue
        ln = p[o + 1]
        if code == 53 and ln >= 1:
            return p[o + 2], p
        o += 2 + ln
    return None, None


state = "wait_discover"
renewals = 0
while time.time() < DEADLINE:
    for fr in poll_frames():
        mt, p = msg_type_of(fr)
        if mt is None:
            continue
        if mt == 1 and state == "wait_discover":
            send(dhcp_frame(p, 2))  # OFFER
            state = "wait_request"
        elif mt == 3:
            send(dhcp_frame(p, 5))  # ACK
            if state == "wait_request":
                state = "bound"
            elif state == "bound":
                renewals += 1
                if renewals >= 2:
                    done("PEER OK")
    time.sleep(0.1)

done(f"PEER FAIL: state={state} renewals={renewals}")
