"""Verify that the LAN secondary params survive a daemon restart.

Run: python lanparam_persist.py [bmc-ip]
The caller must restart the daemon between the two phases (--phase write|read).
"""
import sys

from pyghmi.ipmi import command

BMC = sys.argv[1] if len(sys.argv) > 1 else "192.168.31.72"
PHASE = sys.argv[2] if len(sys.argv) > 2 else "write"
USER = "root"
PASS = "51L1C0M201B"


def raw(c, cmd, data):
    return c.xraw_command(netfn=0x0C, command=cmd, data=list(data))


def get(c, p):
    # the response starts with the parameter revision byte (0x11), then the value
    return list(raw(c, 0x02, [1, p]).get("data", []))[1:]


def setp(c, p, data):
    return raw(c, 0x01, [1, p] + list(data))["code"]


def main():
    c = command.Command(bmc=BMC, userid=USER, password=PASS)

    if PHASE == "write":
        print("  vlan      before %s" % get(c, 20))
        print("  write 7   -> 0x%02x" % setp(c, 20, [7, 0]))
        print("  vlan      after  %s" % get(c, 20))
        assert get(c, 20) == [7, 0], "vlan did not read back from the config tree"
        print("  community -> 'v150rc1'")
        print("  write     -> 0x%02x" % setp(c, 16, b"v150rc1"))
        assert b"v150rc1" in bytes(get(c, 16)).rstrip(b"\x00")
        print("  arp ctrl  write 3 -> 0x%02x" % setp(c, 10, [3]))
        print("  gratuitous write 1 -> 0x%02x" % setp(c, 11, [1]))
        print("  auth en   write 01 03 05 07 0f -> 0x%02x" % setp(c, 2, [1, 3, 5, 7, 15]))
        print("  PHASE 1 OK - restart the daemon now")
    elif PHASE == "read":
        v = get(c, 20)
        com = bytes(get(c, 16)).rstrip(b"\x00")
        arp = get(c, 10)
        garp = get(c, 11)
        auth = get(c, 2)
        print("  vlan          %s" % v)
        print("  community     %r" % com)
        print("  arp control   %s" % arp)
        print("  gratuitous    %s" % garp)
        print("  auth enables  %s" % auth)
        assert v == [7, 0], "vlan lost across restart"
        assert b"v150rc1" in com, "community lost across restart"
        assert arp == [3], "arp control lost across restart"
        assert garp == [1], "gratuitous arp lost across restart"
        assert auth == [1, 3, 5, 7, 15], "auth enables lost across restart"
        print("  PHASE 2 OK - values survived the restart")
    elif PHASE == "restore":
        setp(c, 20, [0, 0])
        setp(c, 16, b"public")
        setp(c, 10, [0x02])          # product default: answer ARP, no gratuitous ARP
        setp(c, 11, [0])
        setp(c, 2, [0x15] * 5)
        print("  vlan          %s" % get(c, 20))
        print("  community     %r" % bytes(get(c, 16)).rstrip(b"\x00"))
        print("  arp control   %s" % get(c, 10))
        print("  gratuitous    %s" % get(c, 11))
        print("  auth enables  %s" % get(c, 2))
        print("  DEFAULTS RESTORED")


if __name__ == "__main__":
    main()
