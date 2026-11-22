#!/bin/sh
# Replace the production U-Boot image on the eMMC boot partition (p1).
#
#   ./flash-uboot.sh -n /var/images/u-boot.img      # dry run: only report
#   ./flash-uboot.sh /var/images/u-boot.img         # backup + write + verify
#
# What it touches: /mnt/p1/u-boot.img only.
# What it never touches: MLO, boot0.scr, boot1.scr, the A/B flags at
# 0x10000 / 0x11000, or any of the rootfs partitions.
#
# Safety properties, in order:
#   1. refuses if the new file is not a FIT image (magic 0xd00dfeed)
#   2. copies the current u-boot.img to /var/images/uboot-backup/<timestamp>
#      and keeps /mnt/p1/u-boot.img.prod as an on-partition backup
#   3. writes to u-boot.img.new, syncs, and only renames after the
#      sha256 of the written file matches the source
#   4. never overwrites u-boot.img.prod if it already exists
#
# The original MLO stays in place on purpose: the ROM finds it first, so a
# broken u-boot.img can never turn into a ROM-level brick.

set -e

MNT=/mnt/p1
DEV=/dev/mmcblk0p1
BACKUP_DIR=/var/images/uboot-backup
dry=0

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        -n|--dry-run) dry=1; shift ;;
        -h|--help)    usage ;;
        *)            src=$1; shift ;;
    esac
done
[ -n "$src" ] || usage
[ -f "$src" ] || { echo "missing source: $src" >&2; exit 1; }

# --- 1. is it really a FIT image? -------------------------------------------
magic=$(dd if="$src" bs=4 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')
[ "$magic" = "d00dfeed" ] || {
    echo "REFUSED: $src does not start with the FIT magic d00dfeed (got $magic)" >&2
    exit 1
}
src_size=$(wc -c < "$src" | tr -d ' ')
echo "source        $src ($src_size bytes, FIT)"

# --- mount p1 if needed -----------------------------------------------------
mounted=0
if ! mountpoint -q "$MNT" 2>/dev/null && ! grep -q " $MNT " /proc/mounts; then
    mkdir -p "$MNT"
    mount "$DEV" "$MNT"
    mounted=1
fi
trap '[ $mounted -eq 1 ] && umount "$MNT" || true' EXIT

cur_size=0
[ -f "$MNT/u-boot.img" ] && cur_size=$(wc -c < "$MNT/u-boot.img" | tr -d ' ')
echo "current       $MNT/u-boot.img ($cur_size bytes)"

# --- 2. backups -------------------------------------------------------------
if [ $dry -eq 1 ]; then
    echo "DRY RUN: would back up to $BACKUP_DIR/u-boot.img.\$(date +%Y%m%d-%H%M%S)"
    echo "DRY RUN: would keep   $MNT/u-boot.img.prod (if absent)"
    echo "DRY RUN: would write  $MNT/u-boot.img.new then rename after sha256 check"
    exit 0
fi

mkdir -p "$BACKUP_DIR"
stamp=$(date +%Y%m%d-%H%M%S)
if [ -f "$MNT/u-boot.img" ]; then
    cp "$MNT/u-boot.img" "$BACKUP_DIR/u-boot.img.$stamp"
    sync
    echo "backup        $BACKUP_DIR/u-boot.img.$stamp"
fi
if [ ! -f "$MNT/u-boot.img.prod" ] && [ -f "$MNT/u-boot.img" ]; then
    cp "$MNT/u-boot.img" "$MNT/u-boot.img.prod"
    sync
    echo "backup        $MNT/u-boot.img.prod (on-partition copy)"
fi

# --- 3. write + verify ------------------------------------------------------
cp "$src" "$MNT/u-boot.img.new"
sync
sum_src=$(sha256sum "$src" | cut -d' ' -f1)
sum_new=$(sha256sum "$MNT/u-boot.img.new" | cut -d' ' -f1)
echo "sha256 src    $sum_src"
echo "sha256 read   $sum_new"
[ "$sum_src" = "$sum_new" ] || { echo "ABORT: readback mismatch, u-boot.img untouched" >&2; exit 1; }

mv "$MNT/u-boot.img.new" "$MNT/u-boot.img"
sync
echo "written       $MNT/u-boot.img"
sha256sum "$MNT/u-boot.img"

cat <<'EOF'

Done. Nothing else changed: MLO, boot0.scr/boot1.scr and both A/B flags
(0x10000 boot / 0x11000 upgrade) are untouched, so the next boot still
goes to the same bank -- it just talks to the serial console this time.

Roll back with:
  cp /mnt/p1/u-boot.img.prod /mnt/p1/u-boot.img.new && sync &&
  mv /mnt/p1/u-boot.img.new /mnt/p1/u-boot.img && sync
or from a silent U-Boot prompt:
  fatload mmc 1:1 0x81000000 /u-boot.img.prod; fatwrite mmc 1:1 0x81000000 u-boot.img $filesize; reset

Then: reboot
EOF
