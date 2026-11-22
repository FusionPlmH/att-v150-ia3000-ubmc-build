"""Read/write the Set-LAN-Config parameters that used to return 0xCC.

Run: python lanparam_test.py [bmc-ip]
"""
import sys

from pyghmi.ipmi import command

BMC = sys.argv[1] if len(sys.argv) > 1 else "192.168.31.72"
USER = "root"
PASS = "51L1C0M201B"

NAMES = {
    0: "set-in-progress",
    1: "auth-type-support",
    2: "auth-type-enable",
    3: "ip-address",
    4: "ip-address-source",
    5: "mac-address",
    6: "subnet-mask",
    10: "bmc-arp-control",
    11: "gratuitous-arp",
    12: "default-gateway",
    16: "community-string",
    17: "num-destinations",
    20: "8021q-vlan-id",
    21: "8021q-vlan-priority",
}


def show(c, params):
    for p in params:
        rsp = c.xraw_command(netfn=0x0C, command=0x02, data=[1, p])
        data = rsp["data"]
        print("  param %-2d %-20s %s" % (p, NAMES.get(p, "?"), " ".join("%02x" % b for b in data)))
        if p == 16:
            print("           as text: %r" % bytes(data).rstrip(b"\x00").decode("ascii", "replace"))


def setp(c, p, data):
    rsp = c.xraw_command(netfn=0x0C, command=0x01, data=[1, p] + list(data))
    print("  set param %-2d %-20s -> code %02x" % (p, NAMES.get(p, "?"), rsp["code"]))


def main():
    c = command.Command(bmc=BMC, userid=USER, password=PASS)
    print("== current ==")
    show(c, [1, 2, 10, 11, 16, 20, 21])

    print("\n== write: auth enables = 1f 1f 1f 1f 1f ==")
    setp(c, 2, [0x1F] * 5)
    show(c, [2])

    print("\n== write: community = 'v150lab' ==")
    setp(c, 16, b"v150lab")
    show(c, [16])

    print("\n== write: arp control = 0x02, gratuitous arp = 0x01 ==")
    setp(c, 10, [0x02])
    setp(c, 11, [0x01])
    show(c, [10, 11])

    print("\n== restore defaults ==")
    setp(c, 2, [0x15] * 5)
    setp(c, 16, b"public")
    setp(c, 10, [0x00])
    setp(c, 11, [0x00])
    show(c, [2, 10, 11, 16])


if __name__ == "__main__":
    main()
