#!/usr/bin/env python
"""Wrap an already-compressed cpio payload in a U-Boot uImage header.

Rebuilds the 64-byte legacy uImage header that mkimage produces, so a rootfs
ramdisk can be repacked on a box that has no mkimage binary.  Compatible with
Python 2.6+ (the uBMC rootfs only has 2.7).

usage:
  uimage-wrap.py verify <image>                    # check header + data CRC
  uimage-wrap.py wrap <payload.gz> <template> <out> # reuse template's fields
  uimage-wrap.py info <image>

Layout (all big endian):
  0x00 magic 0x27051956   0x04 header crc     0x08 timestamp
  0x0c data size          0x10 load address   0x14 entry point
  0x18 data crc           0x1c os   0x1d arch  0x1e type  0x1f comp
  0x20 name (32 bytes, NUL padded)
"""
import struct
import sys

# zlib is not built into this rootfs's python2.7, but binascii is and its
# crc32() is the same function.
try:
    import zlib
    crc32 = lambda b: zlib.crc32(b) & 0xffffffff      # noqa: E731
except ImportError:
    import binascii
    crc32 = lambda b: binascii.crc32(b) & 0xffffffff  # noqa: E731

MAGIC = 0x27051956
HDRLEN = 64
OS_LINUX = 5
ARCH_ARM = 2
TYPE_RAMDISK = 3


def read_image(path):
    with open(path, "rb") as f:
        data = f.read()
    return parse_header(data[:HDRLEN]), data[HDRLEN:]


def parse_header(hdr):
    if len(hdr) < HDRLEN:
        raise ValueError("short header")
    (magic, hcrc, stamp, size, load, entry, dcrc,
     os_, arch, typ, comp) = struct.unpack(">IIIIIIIBBBB", hdr[:32])
    if magic != MAGIC:
        raise ValueError("bad magic 0x%08x" % magic)
    return {
        "magic": magic, "hcrc": hcrc, "timestamp": stamp, "size": size,
        "load": load, "entry": entry, "dcrc": dcrc, "os": os_, "arch": arch,
        "type": typ, "comp": comp, "name": hdr[32:64].split(b"\x00")[0],
    }


def build_header(h, size, dcrc):
    name = h["name"][:31]
    hdr = struct.pack(">IIIIIIIBBBB", MAGIC, 0, h["timestamp"], size,
                      h["load"], h["entry"], dcrc, h["os"], h["arch"],
                      h["type"], h["comp"]) + name + b"\x00" * (32 - len(name))
    hcrc = crc32(hdr)
    return hdr[:4] + struct.pack(">I", hcrc) + hdr[8:]


def describe(h, size, dcrc):
    print("  magic          0x%08x" % MAGIC)
    print("  header crc     0x%08x" % h["hcrc"])
    print("  data size      %d" % size)
    print("  load/entry     0x%08x / 0x%08x" % (h["load"], h["entry"]))
    print("  data crc       0x%08x" % dcrc)
    print("  os/arch/type   0x%02x / 0x%02x / 0x%02x (Linux / ARM / RAMDisk)"
          % (h["os"], h["arch"], h["type"]))
    print("  compression    0x%02x (0 = none, already gzipped)" % h["comp"])
    print("  name           %r" % h["name"])


def verify(path):
    h, payload = read_image(path)
    ok = True
    hdr_recomputed = build_header(h, h["size"], h["dcrc"])
    if struct.unpack(">I", hdr_recomputed[4:8])[0] != h["hcrc"]:
        print("  HEADER CRC MISMATCH: stored 0x%08x expected 0x%08x"
              % (h["hcrc"], struct.unpack(">I", hdr_recomputed[4:8])[0]))
        ok = False
    if len(payload) != h["size"]:
        print("  SIZE MISMATCH: header says %d, file holds %d"
              % (h["size"], len(payload)))
        ok = False
    dcrc = crc32(payload)
    if dcrc != h["dcrc"]:
        print("  DATA CRC MISMATCH: stored 0x%08x expected 0x%08x"
              % (h["dcrc"], dcrc))
        ok = False
    if b"\x1f\x8b" != payload[:2]:
        print("  WARNING: payload does not start with a gzip magic")
    describe(h, h["size"], dcrc)
    print("  VERIFY %s" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


def wrap(payload_path, template_path, out_path):
    h, _ = read_image(template_path)
    with open(payload_path, "rb") as f:
        payload = f.read()
    dcrc = crc32(payload)
    hdr = build_header(h, len(payload), dcrc)
    with open(out_path, "wb") as f:
        f.write(hdr)
        f.write(payload)
    print("  wrote %s (%d + %d bytes)" % (out_path, HDRLEN, len(payload)))
    print("  data crc 0x%08x" % dcrc)
    return 0


def info(path):
    h, payload = read_image(path)
    describe(h, len(payload), crc32(payload))
    return 0


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "verify":
        return verify(argv[2])
    if cmd == "info":
        return info(argv[2])
    if cmd == "wrap" and len(argv) == 5:
        return wrap(argv[2], argv[3], argv[4])
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
