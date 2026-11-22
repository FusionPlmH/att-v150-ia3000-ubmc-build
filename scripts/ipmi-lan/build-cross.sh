#!/bin/bash
# Cross-build ubmc_ipmi_lan for the ATT-V150 uBMC straight from WSL.
#
#   wsl -u root -e bash /mnt/c/.../ubmc-build/scripts/ipmi-lan/build-cross.sh
#
# Why a static binary
# -------------------
# The uBMC root filesystem is a ramdisk (root=/dev/ram0), i.e. everything in
# /usr is recreated on every boot.  The buildroot tree that used to produce the
# daemon therefore does not survive a reboot, and the image's own libc is glibc
# 2.21 while any modern cross toolchain wants 2.34+.  A statically linked
# binary sidesteps both problems: no toolchain has to live on the box, and no
# runtime library has to match.
#
# Prerequisites (WSL, Ubuntu 24.04):
#   dpkg --add-architecture armhf
#   apt install gcc-arm-linux-gnueabihf libssl-dev:armhf
#   # armhf packages come from ports.ubuntu.com; see ubuntu-ports.sources
#
# Output: ubmc-build/prebuilt/ubmc_ipmi_lan (+ .gz for transfers)
set -e

here=$(cd "$(dirname "$0")" && pwd)
out=$(cd "$here/../.." && pwd)/prebuilt

gcc=${CROSS_GCC:-arm-linux-gnueabihf-gcc-13}
strip=${CROSS_STRIP:-arm-linux-gnueabihf-strip}
libcrypto=${LIBCRYPTO_A:-/usr/lib/arm-linux-gnueabihf/libcrypto.a}

command -v "$gcc" > /dev/null || { echo "missing $gcc (see the header of this file)"; exit 1; }
[ -f "$libcrypto" ] || { echo "missing $libcrypto (apt install libssl-dev:armhf)"; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp "$here/ubmc_ipmi_lan.c" "$tmp/"

# -static pulls in libc.a and libcrypto.a; the three "Using 'dlopen' in
# statically linked applications" warnings come from libcrypto code we never
# call (DSO lookup, BIO address parsing) and are harmless.
"$gcc" -O2 -Wall -Wextra -static \
    -I/usr/include/arm-linux-gnueabihf \
    -o "$tmp/ubmc_ipmi_lan" "$tmp/ubmc_ipmi_lan.c" \
    "$libcrypto"
"$strip" "$tmp/ubmc_ipmi_lan"
gzip -9 -c "$tmp/ubmc_ipmi_lan" > "$tmp/ubmc_ipmi_lan.gz"

mkdir -p "$out"
cp "$tmp/ubmc_ipmi_lan" "$tmp/ubmc_ipmi_lan.gz" "$out"/
ls -l "$out/ubmc_ipmi_lan" "$out/ubmc_ipmi_lan.gz"
