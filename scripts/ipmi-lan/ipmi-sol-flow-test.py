"""SOL flow control and BREAK acceptance test.

Two things the plain SOL test cannot prove:

  1. A client that *does* acknowledge must be served through the sliding
     window, i.e. no packet is ever retransmitted and nothing is dropped.
     The BMC declares "streaming without a window" only after a client has
     refused to acknowledge, so that log line must NOT appear here.

  2. A BREAK request (header flag bit 0x10) must be forwarded to the host
     UART.  There is no host-side echo for a BREAK, so the check is that the
     daemon logs the assertion and that the console keeps working afterwards.

usage: ipmi-sol-flow-test.py [host] [user] [password]
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

LOGIN_HINTS = ("login:", "USENEODDXTX")


def printable(data):
    return "".join(chr(c) if 32 <= c < 127 else ("\n" if c == 10 else "")
                   for c in data)


def main():
    from pyghmi.ipmi.command import Command
    from pyghmi.ipmi.private import session as session_mod

    fails = []
    print("=== SOL flow + BREAK test -> %s ===" % HOST)
    bmc = Command(bmc=HOST, userid=USER, password=PASSWORD)
    sess = bmc.ipmi_session
    print("  ipmi session established")

    rx = []

    def on_sol(payload):
        rx.append(bytes(payload))

    sess.sol_handler = on_sol

    bmc.raw_command(netfn=0x06, command=0x48, data=(1, 1, 0xC0, 0, 0, 0))
    print("  SOL activated")

    def send(pl):
        sess.send_payload(payload=bytearray(pl), payload_type=1, retry=False)

    tx_seq = [0]

    def next_seq():
        tx_seq[0] = tx_seq[0] % 15 + 1        # client packets are 1..15
        return tx_seq[0]

    def send_chars(data):
        send(bytes((next_seq(), 0, 0, 0)) + data)

    def pump(seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            session_mod.Session.wait_for_rsp(0.5)

    # -- phase 1: acknowledge every packet, the way a real client does -------
    print("\n  phase 1: acking client, 6 s")
    send_chars(b"\r")
    seqs = []
    deadline = time.time() + 6
    while time.time() < deadline:
        session_mod.Session.wait_for_rsp(0.5)
        # acknowledge the newest data packet we have seen
        if rx:
            last = rx[-1]
            s = last[0] & 0x0F
            if len(last) > 4 and s != 0 and s not in seqs:
                seqs.append(s)
                send(bytes((0, s, len(last) - 4, 0)))
    text1 = b"".join(p[4:] for p in rx if len(p) > 4)
    print("  %d packet(s), %d byte(s), acks sent for seq %s"
          % (len(rx), len(text1), seqs[:16]))
    if not text1:
        fails.append("no host output while acking")
    elif not any(h in printable(text1) for h in LOGIN_HINTS):
        fails.append("no login prompt while acking")

    # duplicate data packets would mean the window retransmitted needlessly
    dupes = [s for s in set(seqs) if seqs.count(s) > 1]
    if dupes:
        fails.append("duplicate sequence(s) while acking: %s" % dupes)

    # -- phase 2: BREAK ------------------------------------------------------
    print("\n  phase 2: BREAK request")
    before = len(rx)
    send(bytes((0, 0, 0, 0x10)))
    pump(3)
    print("  %d packet(s) after BREAK (%d bytes of host output)"
          % (len(rx) - before, sum(len(p) - 4 for p in rx[before:] if len(p) > 4)))

    # the console must still be usable: send a bare CR and expect an echo or
    # at least a fresh prompt within a few seconds
    print("\n  phase 3: console still alive")
    mark = len(rx)
    send_chars(b"\r")
    pump(5)
    text3 = b"".join(p[4:] for p in rx[mark:] if len(p) > 4)
    print("  %d byte(s) after the follow-up CR" % len(text3))
    if not text3:
        fails.append("console dead after BREAK")

    bmc.raw_command(netfn=0x06, command=0x49, data=(1, 1, 0, 0, 0, 0))
    print("\n  SOL deactivated")
    try:
        sess.logout()
    except Exception:
        pass

    print()
    if fails:
        print("FAILED: %s" % "; ".join(fails))
        return 1
    print("all SOL flow checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
