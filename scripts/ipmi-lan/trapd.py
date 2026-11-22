"""Listen for SNMP traps on udp/162 and decode just enough to sanity-check PET.

usage: trapd.py [seconds]
Prints the raw packet, then walks the outer BER structure and lists the
varbind OIDs it can decode, so a malformed encoder shows up immediately.
"""
import socket
import sys
import time

BUDGET = float(sys.argv[1]) if len(sys.argv) > 1 else 60

OIDS = {
    "1.3.6.1.2.1.1.3.0": "sysUpTime.0",
    "1.3.6.1.6.3.1.1.4.1.0": "snmpTrapOID.0",
    "1.3.6.1.4.1.3183.1.1.1.2.0": "petHostname",
    "1.3.6.1.4.1.3183.1.1.1.3.0": "petDateTime",
    "1.3.6.1.4.1.3183.1.1.1.4.0": "petSequenceNumber",
    "1.3.6.1.4.1.3183.1.1.1.5.0": "petEventType",
    "1.3.6.1.4.1.3183.1.1.1.7.0": "petEventSourceType",
    "1.3.6.1.4.1.3183.1.1.1.8.0": "petEventSeverity",
    "1.3.6.1.4.1.3183.1.1.1.9.0": "petSensorType",
    "1.3.6.1.4.1.3183.1.1.1.10.0": "petSensorNumber",
    "1.3.6.1.4.1.3183.1.1.1.11.0": "petEventData",
    "1.3.6.1.4.1.3183.1.1.1.14.0": "petEventDescription",
}


def parse_tlv(buf, i):
    tag = buf[i]
    n = buf[i + 1]
    i += 2
    if n & 0x80:
        k = n & 0x7f
        n = int.from_bytes(buf[i:i + k], "big")
        i += k
    return tag, buf[i:i + n], i + n


def decode_oid(b):
    if not b:
        return ""
    first = b[0]
    arcs = [first // 40, first % 40]
    val = 0
    for byte in b[1:]:
        val = (val << 7) | (byte & 0x7f)
        if not byte & 0x80:
            arcs.append(val)
            val = 0
    return ".".join(str(a) for a in arcs)


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 162))
    s.settimeout(2.0)
    print("listening on udp/162 for %.0fs" % BUDGET, flush=True)
    end = time.time() + BUDGET
    got = 0
    while time.time() < end:
        try:
            data, peer = s.recvfrom(4096)
        except socket.timeout:
            continue
        got += 1
        print("\n=== trap %d from %s (%d bytes) ===" % (got, peer, len(data)), flush=True)
        print("hex:", data.hex(), flush=True)
        try:
            tag, body, _ = parse_tlv(data, 0)
            print("outer tag 0x%02x len %d" % (tag, len(body)))
            i = 0
            t, ver, i = parse_tlv(body, i)
            t, comm, i = parse_tlv(body, i)
            print("version=%d community=%s" % (int.from_bytes(ver, "big"), comm.decode()))
            t, pdu, i = parse_tlv(body, i)
            print("pdu tag 0x%02x (0xa4 = SNMPv1 trap)" % t)
            j = 0
            t, ent, j = parse_tlv(pdu, j)
            print("enterprise oid:", decode_oid(ent))
            t, agent, j = parse_tlv(pdu, j)
            print("agent-addr:", ".".join(str(b) for b in agent))
            t, gen, j = parse_tlv(pdu, j)
            t, spec, j = parse_tlv(pdu, j)
            print("generic=%d specific=%d" % (int.from_bytes(gen, "big"),
                                              int.from_bytes(spec, "big")))
            t, ticks, j = parse_tlv(pdu, j)
            print("uptime ticks:", int.from_bytes(ticks, "big"))
            t, vbl, j = parse_tlv(pdu, j)
            k = 0
            while k < len(vbl):
                t, vb, k = parse_tlv(vbl, k)
                m = 0
                t2, oid, m = parse_tlv(vb, m)
                t3, val, m = parse_tlv(vb, m)
                name = decode_oid(oid)
                shown = val.hex() if t3 != 0x04 else val.decode(errors="replace")
                print("  %-22s %s = %s" % (name, OIDS.get(name, "?"), shown))
        except Exception as exc:
            print("PARSE FAILED:", type(exc).__name__, exc, flush=True)
    print("\ntraps received: %d" % got)
    return 0


if __name__ == "__main__":
    sys.exit(main())
