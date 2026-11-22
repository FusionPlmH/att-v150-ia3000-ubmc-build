"""Poll the BMC's udp/623 from this machine and report when it first answers.

usage: poll623.py <host> [max-seconds] [interval-seconds]
Exits 0 on first reply, 1 if the budget runs out.
"""
import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.31.72"
BUDGET = float(sys.argv[2]) if len(sys.argv) > 2 else 900
INTERVAL = float(sys.argv[3]) if len(sys.argv) > 3 else 10


def request():
    rmcp = bytes([0x06, 0x00, 0xFF, 0x07])
    session = bytes([0x00]) + bytes(8) + bytes([9])
    body = bytearray([0x20, 0x18, 0x00, 0x81, 0x04, 0x38, 0x8E, 0x04, 0x00])
    body[2] = (0x100 - (body[0] + body[1])) & 0xFF
    body[8] = (0x100 - sum(body[3:8])) & 0xFF
    return rmcp + session + bytes(body)


def probe(timeout=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.connect((HOST, 623))
        s.send(request())
        reply = s.recv(2048)
        return len(reply)
    except Exception:
        return 0
    finally:
        s.close()


def main():
    start = time.time()
    n = 0
    while time.time() - start < BUDGET:
        n += 1
        got = probe()
        stamp = time.strftime("%H:%M:%S")
        if got:
            print("%s  ANSWERED (%d B) after %.0fs / %d polls"
                  % (stamp, got, time.time() - start, n), flush=True)
            return 0
        print("%s  poll %d: no reply" % (stamp, n), flush=True)
        time.sleep(INTERVAL)
    print("gave up after %.0fs" % (time.time() - start), flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
