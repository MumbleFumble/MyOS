#!/usr/bin/env python3
"""One-shot HTTP/1.0 server for scripts/test_http.sh.

Listens on 0.0.0.0:80 (the guest reaches it as 10.0.2.2 under
slirp). Logs the request line, replies with a fixed body, closes.
"""
import socket
import sys

BODY = b"MYOS-HTTP-OK phase nine!\n"
RESP = (b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
        + b"Content-Length: " + str(len(BODY)).encode() + b"\r\n\r\n" + BODY)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", 80))
srv.listen(1)
srv.settimeout(120)
conn, addr = srv.accept()
conn.settimeout(10)
data = b""
try:
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            break
        data += chunk
except socket.timeout:
    pass
print("REQUEST:", data.split(b"\r\n")[0].decode("latin1"), flush=True)
conn.sendall(RESP)
conn.close()
print("SERVED", flush=True)
sys.exit(0)
