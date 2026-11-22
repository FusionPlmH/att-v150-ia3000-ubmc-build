"""Acceptance test for the uBMC IPMI-over-LAN listener, driven through pyghmi.

Exercises the parts of IPMI 2.0 that a monitoring tool actually uses: RMCP+
session establishment (RAKP 1-4), encrypted/authenticated payloads, chassis
status/control, and Device ID.

usage: ipmi-client-test.py [host] [user] [password] [--power-on] [--trace]
"""
import os
import sys
import traceback

args = [a for a in sys.argv[1:] if not a.startswith("--")]
FLAGS = {a for a in sys.argv[1:] if a.startswith("--")}

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
        print("  %-26s FAIL  %s: %s" % (label, type(exc).__name__, exc))
        if "--trace" in FLAGS:
            traceback.print_exc()
        FAILS.append(label)
        return None
    if want is not None and got != want:
        print("  %-26s MISMATCH want=%r" % (label, want))
        FAILS.append(label)
    else:
        print("  %-26s %r" % (label, got))
    return got


def main():
    from pyghmi.ipmi.command import Command

    print("=== pyghmi -> %s (user %s) ===" % (HOST, USER))
    bmc = Command(bmc=HOST, userid=USER, password=PASSWORD)
    print("  RMCP+ session established (RAKP1-4 ok)\n")

    check("get_power()", bmc.get_power, {"powerstate": "on"})

    devid = check("Get Device ID (App 0x01)",
                  lambda: bmc.raw_command(netfn=0x06, command=0x01))
    if devid and len(devid["data"]) >= 4:
        d = devid["data"]
        print("      device id %02x  fw %d.%02d  ipmi %d.%d"
              % (d[0], d[1] & 0x7F, d[2], d[3] & 0x0F, d[3] >> 4))

    check("Get Chassis Caps (0x00 0x00)",
          lambda: bmc.raw_command(netfn=0x00, command=0x00))
    check("Get Chassis Status (0x00 0x01)",
          lambda: bmc.raw_command(netfn=0x00, command=0x01))

    if "--power-on" in FLAGS:
        print()
        check("set_power('on')", lambda: bmc.set_power("on") or "sent")
        check("get_power() again", bmc.get_power, {"powerstate": "on"})

    try:
        bmc.ipmi_session.logout()
    except Exception:
        pass

    print()
    if FAILS:
        print("FAILED: %s" % ", ".join(FAILS))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
