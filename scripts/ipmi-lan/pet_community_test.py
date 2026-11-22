"""Check that a PET trap is sent with the configured SNMP community string.

Run: python pet_community_test.py [bmc-ip] [trap-host]
"""
import subprocess
import sys
import time

from pyghmi.ipmi import command

BMC = sys.argv[1] if len(sys.argv) > 1 else "192.168.31.72"
TRAP_HOST = sys.argv[2] if len(sys.argv) > 2 else "192.168.31.52"
USER = "root"
PASS = "51L1C0M201B"
COMMUNITY = b"v150pet"


def lan_set(c, param, data):
    return c.xraw_command(netfn=0x0C, command=0x01,
                          data=[1, param] + list(data))["code"]


def main():
    c = command.Command(bmc=BMC, userid=USER, password=PASS)
    ip = [int(x) for x in TRAP_HOST.split(".")]

    print("  community -> %r" % COMMUNITY.decode())
    print("  write -> 0x%02x" % lan_set(c, 16, COMMUNITY))
    print("  alert dest 1 type=PET -> 0x%02x" % lan_set(c, 18, [1, 1, 0, 0]))
    print("  alert dest 1 addr=%s -> 0x%02x" % (TRAP_HOST, lan_set(c, 19, [1] + ip)))

    here = __file__.rsplit("\\", 1)[0]
    rx = subprocess.Popen([sys.executable, here + "\\trapd.py", "20"],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True)
    time.sleep(2.5)
    print("  alert immediate -> 0x%02x" % c.xraw_command(
        netfn=0x04, command=0x16, data=[1])["code"])
    out, _ = rx.communicate(timeout=40)
    print(out)
    assert "traps received: 1" in out, "no trap arrived (firewall on udp/162?)"
    assert "community=" + COMMUNITY.decode() in out, \
        "trap did not carry the configured community string"

    print("  restoring community=public -> 0x%02x" % lan_set(c, 16, b"public"))
    print("  disabling the test alert destination -> 0x%02x"
          % lan_set(c, 18, [1, 0, 0, 0]))
    print("PET COMMUNITY CHECK PASSED")


if __name__ == "__main__":
    main()
