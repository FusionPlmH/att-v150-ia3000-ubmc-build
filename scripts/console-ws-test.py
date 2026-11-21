import base64
import json
import os
import select
import socket
import ssl
import struct
import sys
import time

HOST = "192.168.31.72"
PORT = 443
TOKEN = sys.argv[1] if len(sys.argv) > 1 else "E2ETOKEN1234"


def build_frame(payload, opcode=0x1):
    data = payload.encode() if isinstance(payload, str) else payload
    mask = os.urandom(4)
    header = bytes([0x80 | opcode])
    n = len(data)
    if n < 126:
        header += bytes([0x80 | n])
    elif n < 65536:
        header += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return header + mask + masked


def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def read_frame(sock):
    hdr = read_exact(sock, 2)
    if not hdr:
        return None, None
    opcode = hdr[0] & 0x0F
    n = hdr[1] & 0x7F
    if n == 126:
        n = struct.unpack(">H", read_exact(sock, 2))[0]
    elif n == 127:
        n = struct.unpack(">Q", read_exact(sock, 8))[0]
    data = read_exact(sock, n) if n else b""
    return opcode, data


def drain(sock, label, seconds):
    print(f"--- {label} ({seconds}s) ---")
    deadline = time.time() + seconds
    got = 0
    while time.time() < deadline:
        r, _, _ = select.select([sock], [], [], 0.5)
        if not r:
            continue
        op, data = read_frame(sock)
        if op is None:
            print("  [连接关闭]")
            break
        if op == 0x8:
            print(f"  [CLOSE] {data!r}")
            break
        text = data.decode(errors="replace").replace("\x1b", "\\e")
        if len(text) > 300:
            text = text[:300] + "..."
        print(f"  [op={op}] {text!r}")
        got += 1
    print(f"  帧数: {got}")
    return got


ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
ctx.set_ciphers("DEFAULT@SECLEVEL=0")
try:
    ctx.minimum_version = ssl.TLSVersion.TLSv1_1
except Exception:
    pass

raw = socket.create_connection((HOST, PORT), 10)
sock = ctx.wrap_socket(raw, server_hostname=HOST)

key = base64.b64encode(os.urandom(16)).decode()
req = (
    "GET /ws HTTP/1.1\r\n"
    f"Host: {HOST}\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    f"Sec-WebSocket-Key: {key}\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Protocol: tty\r\n"
    "\r\n"
)
sock.sendall(req.encode())
head = b""
while b"\r\n\r\n" not in head:
    c = sock.recv(4096)
    if not c:
        print("服务端断开")
        sys.exit(1)
    head += c
print("握手:", head.split(b"\r\n")[0].decode())

print(f'发送 AuthToken')
sock.sendall(build_frame(json.dumps({"AuthToken": TOKEN})))
drain(sock, "认证后", 4)

print("发送回车到主机串口 (ttyd 输入帧 '0' + CR)")
sock.sendall(build_frame("0\r"))
drain(sock, "回车之后", 6)

sock.close()
