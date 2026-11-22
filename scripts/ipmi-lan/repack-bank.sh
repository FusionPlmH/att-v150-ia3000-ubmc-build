#!/bin/sh
#
# repack-bank.sh -- patch the *installed* bank1 ramdisk image with new files
#
# The uBMC boots a ramdisk (root=/dev/ram0), so /usr is recreated on every
# boot: a daemon dropped into /usr/sbin at runtime does not survive a reboot.
# The buildroot tree that produced the image does not survive either (it lived
# in /root, which is part of that same ramdisk), so the only way to bake a new
# binary into the image is to patch the image that is already on /dev/mmcblk0p3.
#
# This does exactly that: extract, replace the named files, repack, rewrap with
# a uImage header copied from the original, and verify before installing.
# Nothing touches /dev/mmcblk0p3 until every check has passed.
#
# usage: repack-bank.sh <workdir> <version-string>
#
set -e

WORK=${1:-/var/images/r25}
VERSION=${2:-""}
IMG=/mnt/p3/rootfs.cpio.uboot
NEW=$WORK/rootfs.cpio.uboot

# files that get replaced inside the image: <source on the running box>:<path in image>
PAYLOAD_SET="
/config/firmware/ubmc_ipmi_lan:./usr/sbin/ubmc_ipmi_lan
/tmp/S47.image:./etc/init.d/S47is_ipmi_lan
/tmp/selfcheck.image:./usr/sbin/ipmi-selfcheck
"

say() { echo "=== $* ==="; }

say "0. preconditions"
[ -f "$IMG" ] || { echo "!! $IMG missing"; exit 1; }
for pair in $PAYLOAD_SET; do
    src=${pair%%:*}
    [ -f "$src" ] || { echo "!! missing payload $src"; exit 1; }
    echo "    $src  $(stat -c%s "$src") bytes"
done
df -h "$WORK" 2>/dev/null | tail -1

say "1. unpack the current image"
rm -rf "$WORK/root" "$WORK"/*.gz "$WORK"/*.cpio
mkdir -p "$WORK/root"
cd "$WORK"
# the first 64 bytes are the uImage header, the rest is a gzipped newc cpio
dd if="$IMG" of=payload.gz bs=64 skip=1 2>/dev/null
if ! gunzip -c payload.gz > payload.cpio; then
    echo "!! payload does not gunzip"
    exit 1
fi
cd "$WORK/root"
cpio -idmu --no-absolute-filenames < ../payload.cpio > /dev/null 2>&1
cd "$WORK"
echo "    $(wc -c < payload.cpio) bytes of cpio, $(du -sk root | cut -f1) KiB unpacked"

say "2. sanity: is this really our image?"
[ -f root/etc/init.d/S45is_mgmtd ]        || { echo "!! S45is_mgmtd missing"; exit 1; }
[ -x root/usr/bin/is_mgmtd ]              || { echo "!! is_mgmtd missing"; exit 1; }
[ -f root/etc/nginx/nginx.conf ]          || { echo "!! nginx.conf missing"; exit 1; }
grep -q 'iptables-rule/pub-ipmi' root/config/is_mgmtd.conf 2>/dev/null || true
ls root/etc/init.d/ | head -5

say "3. replace files"
for pair in $PAYLOAD_SET; do
    src=${pair%%:*}
    dst=${pair#*:}
    [ -d "$(dirname "$WORK/root/$dst")" ] || { echo "!! $(dirname "$dst") missing in image"; exit 1; }
    install -m 755 "$src" "$WORK/root/$dst"
    echo "    $dst  $(stat -c%s "$WORK/root/$dst") bytes"
done

say "4. init scripts must not keep the runtime IS_ prefix"
for s in "$WORK"/root/etc/init.d/IS_*; do
    [ -e "$s" ] || continue
    echo "!! polluted name $s"
    exit 1
done
rm -f "$WORK"/root/etc/ssh/ssh_host_*
rm -rf "$WORK"/root/var/cache/nginx "$WORK"/root/run/nginx.pid "$WORK"/root/root/.rnd

say "5. repack"
cd "$WORK/root"
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > ../new.cpio.gz
cd "$WORK"
echo "    gzipped payload $(stat -c%s new.cpio.gz) bytes (was $(stat -c%s payload.gz))"

say "6. file list must be identical to the original"
gunzip -c payload.gz | cpio -t 2>/dev/null | sort > old.list
gunzip -c new.cpio.gz | cpio -t 2>/dev/null | sort > new.list
if ! cmp -s old.list new.list; then
    echo "!! file list changed:"
    echo "    old $(wc -l < old.list) entries, new $(wc -l < new.list) entries"
    grep -vxF -f old.list new.list | head -10 | sed 's/^/    only in new: /' || true
    grep -vxF -f new.list old.list | head -10 | sed 's/^/    only in old: /' || true
    exit 1
fi
echo "    $(wc -l < new.list) entries, unchanged"

say "7. wrap a uImage header around it (copied from the original)"
python /tmp/uimage-wrap.py wrap new.cpio.gz "$IMG" "$NEW"
python /tmp/uimage-wrap.py verify "$NEW" || { echo "!! new image failed verify"; exit 1; }

say "8. done -- nothing installed yet"
echo "    new image: $NEW"
echo "    sha256:    $(sha256sum "$NEW" | cut -d' ' -f1)"
[ -n "$VERSION" ] && echo "    version:   $VERSION"
exit 0
