#!/usr/bin/env python
"""Switch the uBMC U-Boot's default `silent` flag without rebuilding it.

The vendor ships `u-boot.img` as a **FIT image** (a DTB, magic 0xd00dfeed)
whose `/images/firmware@1` node points at the raw U-Boot binary with
`data-offset` / `data-size`.  That binary is uncompressed, so the default
environment string `silent=1` sits in it verbatim.  Flipping that one byte
to `0` makes U-Boot print its banner and prompts over the serial console
instead of swallowing every character -- and nothing else changes: same
size, same FIT structure, same load address, no checksum to fix unless the
FIT carries hash nodes (this one does not).

Why not rebuild: the build needs a 2018 Debian stretch chroot and a ~1 GB
Buildroot tree, and the board's root filesystem is a ramdisk so the
toolchain cannot live on the box.  A one-byte patch gets the identical
behaviour with none of that.

usage:
  patch-uboot-silent.py check <u-boot.img>
  patch-uboot-silent.py patch <in.u-boot.img> <out.u-boot.img>
  patch-uboot-silent.py patch --value 1 <in> <out>     # revert

Exit codes: 0 ok, 1 refused/failed, 2 usage.

Python 2.6+ and 3.x (same constraint as uimage-wrap.py: the uBMC rootfs
only has 2.7).
"""
import struct
import sys

MAGIC_FDT = 0xD00DFEED
TOK_BEGIN_NODE = 1
TOK_END_NODE = 2
TOK_PROP = 3
TOK_NOP = 4
TOK_END = 9


def load(path):
    f = open(path, "rb")
    try:
        return f.read()
    finally:
        f.close()


def save(path, data):
    f = open(path, "wb")
    try:
        f.write(data)
    finally:
        f.close()


def u32(data, off):
    return struct.unpack(">I", data[off:off + 4])[0]


class Fit(object):
    """Minimal FIT/DTB walker: nodes with their properties and file offsets."""

    def __init__(self, data):
        if len(data) < 40:
            raise ValueError("too short to be a FIT image")
        magic, totalsize, off_struct, off_strings = struct.unpack(">IIII", data[:16])
        if magic != MAGIC_FDT:
            raise ValueError("not a FIT image (magic 0x%08x, want 0x%08x)"
                             % (magic, MAGIC_FDT))
        if totalsize > len(data):
            raise ValueError("FIT totalsize %d exceeds file size %d"
                             % (totalsize, len(data)))
        self.data = data
        self.totalsize = totalsize
        self.base = (totalsize + 3) & ~3   # U-Boot: external data starts here
        self.nodes = []                    # [(path, {prop: (off, len, val)})]
        self._walk(off_struct, off_strings)

    def _walk(self, off_struct, off_strings):
        data = self.data
        p = off_struct
        stack = []
        while p + 4 <= len(data):
            tok = u32(data, p)
            p += 4
            if tok == TOK_BEGIN_NODE:
                end = data.index(b"\x00", p)
                name = data[p:end].decode("utf-8", "replace")
                p = (end + 4) & ~3
                stack.append(name)
                self.nodes.append(("/" + "/".join([s for s in stack if s]), {}))
            elif tok == TOK_END_NODE:
                if not stack:
                    break
                stack.pop()
                if not stack:
                    break
            elif tok == TOK_PROP:
                length = u32(data, p)
                nameoff = u32(data, p + 4)
                p += 8
                end = data.index(b"\x00", off_strings + nameoff)
                pname = data[off_strings + nameoff:end].decode("utf-8", "replace")
                if stack:
                    self.nodes[-1][1][pname] = (p, length, data[p:p + length])
                p = (p + length + 3) & ~3
            elif tok == TOK_NOP:
                continue
            else:      # TOK_END or garbage
                break

    def node(self, path):
        for name, props in self.nodes:
            if name == path:
                return props
        return None

    def region(self, path):
        """(file_offset, size) of an image node's data, or None."""
        props = self.node(path)
        if props is None:
            return None
        if "data" in props:                      # inline payload
            off, length, _ = props["data"]
            return off, length
        if "data-offset" in props and "data-size" in props:
            doff = u32(props["data-offset"][2] + b"\x00" * 4, 0)
            dsize = u32(props["data-size"][2] + b"\x00" * 4, 0)
            return self.base + doff, dsize
        return None

    def hashes(self):
        """Any `hash` subnode / `value`+`algo` property means we must not
        patch the payload blindly."""
        found = []
        for name, props in self.nodes:
            if "/images/" in "/" + name and ("hash" in name or
                                            ("value" in props and "algo" in props)):
                found.append(name)
        return found


def describe(data, path="<image>"):
    fit = Fit(data)
    print("  file           %s (%d bytes)" % (path, len(data)))
    print("  FIT totalsize  %d (data area starts at 0x%x)" % (fit.totalsize, fit.base))
    for node in ("/images/firmware@1", "/images/fdt@1"):
        r = fit.region(node)
        if r:
            print("  %-20s 0x%06x .. 0x%06x  (%d bytes)"
                  % (node, r[0], r[0] + r[1], r[1]))
    props = fit.node("/images/firmware@1") or {}
    if "description" in props:
        print("  version        %s"
              % props["description"][2].rstrip(b"\x00").decode("utf-8", "replace"))
    h = fit.hashes()
    print("  hash nodes     %s" % ("NONE (nothing to recompute)" if not h else ", ".join(h)))
    return fit


def find_silent(data, off, size):
    """Offsets of every `silent=<n>` inside [off, off+size)."""
    region = data[off:off + size]
    hits = []
    start = 0
    while True:
        i = region.find(b"silent=", start)
        if i < 0:
            break
        hits.append(off + i)
        start = i + 1
    return hits


def cmd_check(path):
    data = load(path)
    fit = describe(data, path)
    reg = fit.region("/images/firmware@1")
    if reg is None:
        print("FAIL: no /images/firmware@1 payload")
        return 1
    hits = find_silent(data, reg[0], reg[1])
    if not hits:
        print("  silent=        NOT FOUND (already rebuilt from source?)")
        return 1
    for h in hits:
        val = data[h + 7:h + 8]
        print("  silent=        '%s' at file offset 0x%X (payload +0x%X)"
              % (val.decode("ascii", "replace"), h, h - reg[0]))
    return 0


def cmd_patch(src, dst, value):
    orig = load(src)
    data = bytearray(orig)
    fit = describe(orig, src)

    hashes = fit.hashes()
    if hashes:
        print("REFUSED: FIT carries hash nodes (%s); patching would invalidate them."
              % ", ".join(hashes))
        return 1

    reg = fit.region("/images/firmware@1")
    if reg is None:
        print("REFUSED: no /images/firmware@1 payload")
        return 1
    hits = find_silent(orig, reg[0], reg[1])
    if len(hits) != 1:
        print("REFUSED: expected exactly 1 'silent=' in the payload, found %d"
              % len(hits))
        return 1

    off = hits[0]
    old = orig[off + 7:off + 8]
    if old not in (b"0", b"1"):
        print("REFUSED: 'silent=%s' is not 0/1" % old)
        return 1
    if old == value:
        print("  already silent=%s, nothing to do" % value.decode())
        save(dst, orig)
        return 0

    data[off + 7] = ord(value)
    out = bytes(data)
    save(dst, out)

    changed = [i for i in range(len(out)) if out[i] != orig[i]]
    print("  patched        silent=%s -> silent=%s at 0x%X"
          % (old.decode(), value.decode(), off))
    print("  bytes changed  %d %s" % (len(changed),
                                      ["0x%X" % c for c in changed]))
    print("  size           %d (unchanged: %s)"
          % (len(out), len(out) == len(orig)))
    print("  wrote          %s" % dst)
    return 0


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "check":
        return cmd_check(argv[2])
    if cmd == "patch":
        args = argv[2:]
        value = b"0"
        if args and args[0] in ("--value", "-v"):
            value = args[1].encode()
            args = args[2:]
        if len(args) != 2 or value not in (b"0", b"1"):
            sys.stderr.write(__doc__)
            return 2
        return cmd_patch(args[0], args[1], value)
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
