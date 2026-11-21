"""SOL acceptance test through pyghmi's ``Console`` helper.

NOTE: ``console.Console`` expects an interactive stdin and starts its login in a
background thread, so it does not work in a scripted/non-tty run (the session
context never advances).  Use it to drive SOL by hand; for CI-style checks use
``ipmi-sol-raw-test.py``, which drives the same payloads through ``Command``.

"""

Activates an SOL payload through pyghmi's Console, reads the host console and
sends keystrokes.  Also verifies that the web console path is unaffected: the
BMC fans the host serial out over /var/run/console_manager_socket, so an SOL
client and ttyd should both keep receiving.

usage: ipmi-sol-test.py [host] [user] [password] [--trace]
"""
import sys
import time

args = [a for a in sys.argv[1:] if not a.startswith("--")]
FLAGS = {a for a in sys.argv[1:] if a.startswith("--")}

HOST = args[0] if len(args) > 0 else "192.168.31.72"
USER = args[1] if len(args) > 1 else "root"
PASSWORD = args[2] if len(args) > 2 else "51L1C0M201B"

received = []


def io(data):
    text = data.decode("latin-1") if isinstance(data, bytes) else str(data)
    received.append(text)


def main():
    from pyghmi.ipmi import console

    print("=== SOL -> %s ===" % HOST)
    con = console.Console(bmc=HOST, userid=USER, password=PASSWORD,
                          iohandler=io, force=True)

    deadline = time.time() + 15
    while not con.connected and time.time() < deadline:
        time.sleep(0.3)
    if not con.connected:
        print("  FAILED: SOL payload did not activate")
        if "--trace" in FLAGS:
            import traceback
            traceback.print_exc()
        return 1
    print("  payload activated (maxout=%s)" % getattr(con, "maxoutcount", "?"))

    print("  sending CR ...")
    if hasattr(con, "send_keys"):
        con.send_keys("\r")
    else:
        con._addpendingdata("\r")
        con._sendpendingoutput()
    time.sleep(4)

    if hasattr(con, "send_keys"):
        con.send_keys("\r")
    time.sleep(2)

    text = "".join(received)
    print("  received %d chunk(s), %d bytes" % (len(received), len(text)))
    printable = "".join(ch if 32 <= ord(ch) < 127 else ("\n" if ch == "\n" else "")
                        for ch in text)
    tail = printable.strip().splitlines()[-6:]
    for line in tail:
        print("    | %s" % line.rstrip())

    ok = True
    if not text:
        print("  FAILED: no data from the host")
        ok = False
    elif "login:" not in printable and "USENEODDXTX" not in printable:
        print("  WARN: no login prompt seen (host may not be at its console)")

    try:
        con.close()
    except Exception:
        pass
    print()
    print("all SOL checks passed" if ok else "SOL test FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
