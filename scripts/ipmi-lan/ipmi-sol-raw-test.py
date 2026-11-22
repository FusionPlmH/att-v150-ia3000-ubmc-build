"""Low-level SOL acceptance test.

pyghmi's ``Console`` helper assumes an interactive stdin and its background
thread does not start reliably in a scripted run, so this drives the SOL
payload directly through the same session object it would use:

  App 0x4A  Get Payload Activation Status
  App 0x48  Activate Payload
  payload type 1 for the actual character stream
  App 0x49  Deactivate Payload

usage: ipmi-sol-raw-test.py [host] [user] [password]
"""
import os
import sys
import time

args = [a for a in sys.argv[1:] if not a.startswith("--")]
args = [a for a in sys.argv[1:] if not a.startswith("--")]
HOST = args[0] if len(args) > 0 else os.environ.get("IPMI_HOST", "")
USER = args[1] if len(args) > 1 else os.environ.get("IPMI_USER", "root")
PASSWORD = args[2] if len(args) > 2 else os.environ.get("IPMI_PASS", "")
if not HOST or not PASSWORD:
    sys.exit("usage: %s <bmc> <user> <password>   (or set IPMI_HOST / IPMI_PASS)"
             % os.path.basename(sys.argv[0]))

FAILS = []


def check(label, fn, want=None):
    try:
        got = fn()
    except Exception as exc:
        print("  %-30s FAIL %s: %s" % (label, type(exc).__name__, exc))
        FAILS.append(label)
        return None
    if want is not None and got != want:
        print("  %-30s MISMATCH %r (want %r)" % (label, got, want))
        FAILS.append(label)
    else:
        print("  %-30s %r" % (label, got))
    return got


def main():
    from pyghmi.ipmi.command import Command

    print("=== SOL raw test -> %s ===" % HOST)
    bmc = Command(bmc=HOST, userid=USER, password=PASSWORD)
    sess = bmc.ipmi_session
    print("  ipmi session established\n")

    check("Get Payload Activation Status",
          lambda: bmc.raw_command(netfn=0x06, command=0x4A, data=(1,))["data"].hex())
    check("Get Payload Instance Info",
          lambda: bmc.raw_command(netfn=0x06, command=0x4B, data=(1, 1))["data"].hex())

    act = check("Activate Payload (SOL)",
                lambda: bmc.raw_command(netfn=0x06, command=0x48,
                                        data=(1, 1, 0xC0, 0, 0, 0)))
    if act is not None:
        d = act["data"]
        maxout = d[4] | (d[5] << 8)
        port = d[8] | (d[9] << 8)
        print("      maxout=%d port=%d" % (maxout, port))
        if port != 623:
            FAILS.append("SOL port is %d, clients require 623" % port)

    # --- data path -------------------------------------------------------
    rx = []

    def on_sol(payload):
        rx.append(bytes(payload))

    sess.sol_handler = on_sol

    from pyghmi.ipmi.private import session as session_mod

    def send_chars(text, seq):
        pl = bytearray((seq, 0, 0, 0)) + text.encode()
        sess.send_payload(payload=pl, payload_type=1, retry=False)

    def pump(seconds):
        """pyghmi's Session is event driven: nothing arrives unless we pump."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            session_mod.Session.wait_for_rsp(0.5)

    print("\n  sending CR through SOL ...")
    send_chars("\r", 1)
    pump(6)

    text = b"".join(p[4:] for p in rx if len(p) > 4)
    print("  got %d SOL packet(s), %d host byte(s)" % (len(rx), len(text)))
    for p in rx[:3]:
        print("      pkt %s | %r" % (p[:4].hex(), p[4:40]))
    printable = "".join(chr(c) if 32 <= c < 127 else ("\n" if c == 10 else "")
                        for c in text)
    tail = [l.rstrip() for l in printable.splitlines() if l.strip()][-8:]
    for line in tail:
        print("      | %s" % line)

    if not text:
        FAILS.append("no host output over SOL")
    elif "login:" not in printable and "USENEODDXTX" not in printable:
        print("      (warning: no login prompt seen)")

    check("Deactivate Payload",
          lambda: bmc.raw_command(netfn=0x06, command=0x49,
                                  data=(1, 1, 0, 0, 0, 0)) and "ok")

    try:
        sess.logout()
    except Exception:
        pass

    print()
    if FAILS:
        print("FAILED: %s" % ", ".join(FAILS))
        return 1
    print("all SOL checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())


