"""Exercise the SDR repository write commands and the LAN secondary params.

Run: python sdr_lan_test.py [bmc-ip]
"""
import os
import sys

from pyghmi.ipmi import command

BMC = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("IPMI_HOST", "")
USER = os.environ.get("IPMI_USER", "root")
PASS = os.environ.get("IPMI_PASS", "")
if not BMC or not PASS:
    sys.exit("usage: %s <bmc> [args]   (or set IPMI_HOST / IPMI_PASS)"
             % os.path.basename(sys.argv[0]))

SDR_USER_ID0 = 0x0100


def raw(c, netfn, cmd, data=()):
    return c.xraw_command(netfn=netfn, command=cmd, data=list(data))


def sdr_info(c):
    r = raw(c, 0x0A, 0x20)
    d = r.get("data", [])
    # pyghmi drops the completion code, so index from 0
    ver = d[0]
    cnt = d[1] | (d[2] << 8)
    free = d[3] | (d[4] << 8)
    ops = d[13] if len(d) > 13 else -1
    print("  count=%d free=%d version=0x%02x op-support=0x%02x n=%d" % (cnt, free, ver, ops, len(d)))
    return cnt


def sdr_dump(c):
    rid = 0
    seen = []
    while True:
        r = raw(c, 0x0A, 0x23, [0, 0, rid & 0xff, rid >> 8, 0, 0xFF])
        d = r.get("data", [])
        if len(d) < 5:
            print("  get-sdr stopped at rid=0x%04x (data=%s)" % (rid, d))
            break
        nxt = d[0] | (d[1] << 8)
        rec = d[2:]
        rec_id = rec[0] | (rec[1] << 8) if len(rec) > 1 else -1
        rtype = rec[3] if len(rec) > 3 else -1
        slen = rec[4] if len(rec) > 4 else -1
        # the ID string starts at offset 48 for a full sensor record
        name = ""
        if rtype == 0x01 and len(rec) > 48:
            name = bytes(b & 0x7F for b in rec[48:48 + (slen - 43)]).decode("ascii", "replace")
        seen.append(rec_id)
        print("  rec id=0x%04x type=0x%02x len=%d name=%r next=0x%04x first=%s"
              % (rec_id, rtype, len(rec), name, nxt,
                 " ".join("%02x" % b for b in rec[:8])))
        if nxt == 0xFFFF:
            break
        rid = nxt
    return seen


def add_sdr(c, name, sensor_num):
    """Build a minimal 50+ byte full sensor record and Add SDR it."""
    nl = len(name)
    rec = bytearray(48 + nl)
    rec[0] = 0x00
    rec[1] = 0x00                      # BMC assigns the real ID
    rec[2] = 0x51
    rec[3] = 0x01                      # full sensor record
    rec[4] = 43 + nl                   # bytes after this field
    rec[5] = 0x20
    rec[6] = 0x00
    rec[7] = sensor_num & 0xFF
    rec[8] = 0x07                      # entity: system board
    rec[9] = 0x01
    rec[11] = 0x40                     # sensor readable
    rec[12] = 0x01                     # temperature
    rec[13] = 0x01                     # threshold based
    rec[20] = 0x01                     # degrees C
    rec[23] = 0x00                     # linear
    rec[24] = 0x01                     # M = 1
    rec[35] = 0xFF                     # maximum reading
    rec[47] = 0xC0 | nl
    rec[48:] = name.encode("ascii")
    r = raw(c, 0x0A, 0x24, rec)
    print("  add SDR %r -> code 0x%02x id=%s" % (name, r["code"], list(r.get("data", []))))
    return r


def del_sdr(c, rid):
    r = raw(c, 0x0A, 0x26, [rid & 0xFF, rid >> 8])
    print("  delete SDR 0x%04x -> code 0x%02x" % (rid, r["code"]))


def clear_sdr(c):
    r = raw(c, 0x0A, 0x27, [0, 0, ord("C"), ord("L"), ord("R"), 0xAA])
    print("  clear SDR repo -> code 0x%02x" % r["code"])


def lan(c, param):
    r = raw(c, 0x0C, 0x02, [1, param])
    return list(r.get("data", []))


def main():
    c = command.Command(bmc=BMC, userid=USER, password=PASS)

    print("== SDR repository, baseline ==")
    clear_sdr(c)                     # start from a known state
    base = sdr_info(c)
    ids = sdr_dump(c)
    print("  %d records iterated" % len(ids))
    assert len(ids) == base, "record count mismatch: info=%d iterated=%d" % (base, len(ids))

    print("\n== Add SDR ==")
    add_sdr(c, "Host Chassis Intr", 0x80)
    after = sdr_info(c)
    assert after == base + 1, "add did not grow the repo"
    ids = sdr_dump(c)
    assert SDR_USER_ID0 in ids, "added record not reachable"

    print("\n== Partial Add SDR (two fragments) ==")
    rec = bytearray(48 + 4)
    rec[2] = 0x51
    rec[3] = 0x01
    rec[4] = 43 + 4
    rec[5] = 0x20
    rec[7] = 0x81
    rec[8] = 0x07
    rec[9] = 0x01
    rec[11] = 0x40
    rec[12] = 0x01
    rec[13] = 0x01
    rec[20] = 0x01
    rec[23] = 0x00
    rec[24] = 0x01
    rec[35] = 0xFF
    rec[47] = 0xC0 | 4
    rec[48:] = b"Half"
    cut = 30
    r = raw(c, 0x0A, 0x25, [0, 0, 0, 1] + list(rec[:cut]))
    print("  fragment 1 -> code 0x%02x" % r["code"])
    r = raw(c, 0x0A, 0x25, [0, 0, cut, 0] + list(rec[cut:]))
    print("  fragment 2 -> code 0x%02x data=%s" % (r["code"], list(r.get("data", []))))
    after2 = sdr_info(c)
    assert after2 == after + 1, "partial add did not grow the repo"

    print("\n== Delete SDR ==")
    del_sdr(c, SDR_USER_ID0)
    after3 = sdr_info(c)
    assert after3 == after2 - 1, "delete did not shrink the repo"
    try:
        raw(c, 0x0A, 0x26, [0x01, 0x00])
        code = 0
    except Exception as exc:          # pyghmi raises on any non-zero code
        code = getattr(exc, "code", None)
        if not code:
            code = 0xCB if "not present" in str(exc) else -1
    print("  delete a generated sensor record -> code 0x%02x (0xcb = not deletable)" % code)
    assert code == 0xCB, "deleting a generated record must fail with 0xcb"

    print("\n== Clear SDR repository ==")
    clear_sdr(c)
    after4 = sdr_info(c)
    assert after4 == base, "clear did not restore the baseline"
    print("  back to %d records" % after4)

    print("\n== LAN secondary params ==")
    for p, name in ((1, "auth-type-support"), (2, "auth-type-enable"),
                    (10, "arp-control"), (11, "gratuitous-arp"),
                    (16, "community"), (20, "vlan"), (21, "vlan-priority")):
        print("  param %-2d %-18s %s" % (p, name, " ".join("%02x" % b for b in lan(c, p))))

    print("\n== LAN param 16 round trip ==")
    r = raw(c, 0x0C, 0x01, [1, 16] + list(b"s1l1c0m"))
    print("  write -> 0x%02x" % r["code"])
    got = bytes(lan(c, 16)).rstrip(b"\x00").decode("ascii", "replace")
    print("  read  -> %r" % got)
    assert "s1l1c0m" in got, "community did not read back"
    raw(c, 0x0C, 0x01, [1, 16] + list(b"public"))

    print("\n== LAN param 20 (VLAN) round trip ==")
    print("  before -> %s" % lan(c, 20))
    r = raw(c, 0x0C, 0x01, [1, 20, 5, 0])
    print("  write 5 -> 0x%02x" % r["code"])
    print("  after  -> %s" % lan(c, 20))
    raw(c, 0x0C, 0x01, [1, 20, 0, 0])
    print("  restored -> %s" % lan(c, 20))

    print("\nALL CHECKS PASSED")


if __name__ == "__main__":
    main()
