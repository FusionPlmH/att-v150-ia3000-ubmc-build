"""Reachability probe for the uBMC IPMI-over-LAN listener.

Sends a well-formed IPMI 1.5 Get Channel Authentication Capabilities request
(the pre-session query every client makes first) and reports whether the BMC
answers.  Uses a connected UDP socket so an ICMP port-unreachable surfaces as
ConnectionRefusedError instead of a silent timeout.

usage: probe623.py [host]
"""
import socket
import os
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("IPMI_HOST", "")
PORT = 623


def get_chan_auth_cap(channel=0x8E, priv=0x04):
    rmcp = bytes([0x06, 0x00, 0xFF, 0x07])
    # auth type 0, seq 0, session 0, 9 byte payload
    session = bytes([0x00]) + bytes(4) + bytes(4) + bytes([9])
    # rsAddr, netFn=App/lun0, chk, rqAddr, rqSeq, cmd=0x38, ch, priv, chk
    body = bytearray([0x20, 0x18, 0x00, 0x81, 0x04, 0x38, channel, priv, 0x00])
    body[2] = (0x100 - (body[0] + body[1])) & 0xFF
    body[8] = (0x100 - sum(body[3:8])) & 0xFF
    return rmcp + session + bytes(body)


def main():
    data = get_chan_auth_cap()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3.0)
    s.connect((HOST, PORT))
    print("probe %s:%d  tx=%s" % (HOST, PORT, data.hex()))
    ok = False
    for attempt in range(3):
        t0 = time.time()
        try:
            s.send(data)
            reply = s.recv(2048)
            ms = (time.time() - t0) * 1000
            print("  attempt %d: REPLY %dB in %.1fms  %s"
                  % (attempt, len(reply), ms, reply.hex()))
            ok = True
        except ConnectionRefusedError:
            print("  attempt %d: ICMP port unreachable (no listener)" % attempt)
        except socket.timeout:
            print("  attempt %d: timeout (packet dropped)" % attempt)
        except Exception as exc:
            print("  attempt %d: %s %s" % (attempt, type(exc).__name__, exc))
        time.sleep(0.3)
    s.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
