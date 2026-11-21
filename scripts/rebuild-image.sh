#!/bin/bash
# 从公开源码的构建产物，重建 bank1 用的 rootfs.cpio.uboot
#
# 用途：把「本地改动 + 从出厂固件移植的升级组件」重新施加到原始产物上，
#       避免手工操作遗漏或污染（rcS 会在运行时把 S49ntp/S50sshd/
#       S50nginx/S50is_web 改名成 IS_*，在树里手工改容易被误伤）。
#
# 前置：
#   /root/uBMC-fork/buildroot/output/images/rootfs.cpio.uboot   原始产物
#   /root/factory-rootfs                                        出厂 rootfs（解开的）
#   /root/libnew.so                                             编译好的 libprod_mgmtd_inst.so
#
# 用法： bash rebuild-image.sh [输出目录]
set -e

SRC_IMG=/root/uBMC-fork/buildroot/output/images/rootfs.cpio.uboot
FACTORY=/root/factory-rootfs
MGMTD_LIB=/root/libnew.so
OUT=${1:-/root/build-fresh}
MKIMAGE=/root/uBMC-fork/buildroot/output/host/bin/mkimage

PUBKEY='ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQCqE4OC9WJ6a7H2iuWe3KbFPe31ewKq91xjVstlgeI9eRIBjG/wXTJhA+ApnFSIi8A5mKsLDtTe3p6yDJ1ql6EjagOy+GpU9nEZIWACVKUFYe2YefNcnf8ZWKdeSD7LIJu7u3vL1hHeRII9Ub/DbJA2INirCxCSAXtSuBOt67jKwsTFZ87LiN/KSq91hiHyVb9sEu9dtJ69P5ULdoy51wo6bhD+CLI/ymvUDhcxJgCo+WjbmZLMjnKZ3gkQhD3jdp+puT1yleUUVcVzKBsLMlCq8Gtw7EmrnqNJG+UkZGx4wDUh/XO13goUD6prTmXHOfOUZwERdJRWnqpuV3Hm0paL ubmc-dump-rsa'

echo "=== 1. 解原始产物 ==="
rm -rf "$OUT"; mkdir -p "$OUT"; cd "$OUT"
python3 - "$SRC_IMG" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
size = struct.unpack('>I', d[12:16])[0]
open('p.gz', 'wb').write(d[64:64+size])
PY
gunzip -c p.gz > p.cpio
mkdir root && cd root && cpio -idmu --no-absolute-filenames < ../p.cpio > /dev/null 2>&1
cd "$OUT"
R="$OUT/root"
echo "    条目数 $(find "$R" | wc -l)"

echo "=== 2. 本地改动 ==="
python3 - "$R/etc/passwd" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace('root:x:0:0:root:/root:/sbin/nologin', 'root:x:0:0:root:/root:/bin/sh')
s = s.replace('sshd:x:1001:1002:SSH drop priv user:/:/bin/false',
              'sshd:x:1001:1002:SSH drop priv user:/var/empty:/bin/false')
open(p, 'w').write(s)
PY

mkdir -p "$R/root/.ssh"
printf '%s\n' "$PUBKEY" > "$R/root/.ssh/authorized_keys"
chmod 700 "$R/root/.ssh"; chmod 600 "$R/root/.ssh/authorized_keys"

V="$R/etc/product/UBMC/version"
echo -n '##UBMC_1.1.0.5_LOCAL_BUILD_bd9de8c##'    > "$V/software_version.txt"
echo -n '##UBMC_FW_1.1.0.5_LOCAL_BUILD_bd9de8c##' > "$V/firmware_version.txt"

cp -f "$MGMTD_LIB" "$R/usr/lib/libprod_mgmtd_inst.so.0.0.0"
cp -f "$MGMTD_LIB" "$R/usr/lib32/libprod_mgmtd_inst.so.0.0.0"

echo "=== 3. 批次一：调试与工具 ==="
for f in valgrind vgdb valgrind-listener valgrind-di-server callgrind_annotate \
         callgrind_control cg_annotate cg_diff cg_merge ms_print \
         ntpq ssh-copy-id json2lua lua2json; do
    cp -a "$FACTORY/usr/bin/$f" "$R/usr/bin/$f"
done
mkdir -p "$R/usr/lib/valgrind" "$R/usr/lib32/valgrind" \
         "$R/usr/lib/lua/5.1" "$R/usr/lib32/lua/5.1" "$R/usr/share/lua/5.1/cjson"
cp -a "$FACTORY/usr/lib/valgrind/."   "$R/usr/lib/valgrind/"
cp -a "$FACTORY/usr/lib32/valgrind/." "$R/usr/lib32/valgrind/"
cp -a "$FACTORY/usr/lib/lua/5.1/cjson.so"  "$R/usr/lib/lua/5.1/cjson.so"
cp -a "$FACTORY/usr/lib32/lua/5.1/cjson.so" "$R/usr/lib32/lua/5.1/cjson.so"
cp -a "$FACTORY/usr/share/lua/5.1/json2lua.lua" \
      "$FACTORY/usr/share/lua/5.1/lua2json.lua" "$R/usr/share/lua/5.1/"
cp -a "$FACTORY/usr/share/lua/5.1/cjson/." "$R/usr/share/lua/5.1/cjson/"

echo "=== 4. 批次二：库升级 ==="
# 4.1 新增 soname（与旧版并存，互不影响）
for rel in \
    usr/lib/libcrypto.so.1.1          usr/lib32/libcrypto.so.1.1 \
    usr/lib/libssl.so.1.1             usr/lib32/libssl.so.1.1 \
    usr/lib/libcurl.so.4.6.0          usr/lib32/libcurl.so.4.6.0 \
    usr/lib/libssh.so.4.8.6           usr/lib32/libssh.so.4.8.6 \
    usr/lib/libnetsnmp.so.40.0.0      usr/lib32/libnetsnmp.so.40.0.0 \
    usr/lib/libnetsnmpagent.so.40.0.0 usr/lib32/libnetsnmpagent.so.40.0.0 \
    usr/lib/libnetsnmphelpers.so.40.0.0 usr/lib32/libnetsnmphelpers.so.40.0.0 \
    usr/lib/libnetsnmpmibs.so.40.0.0  usr/lib32/libnetsnmpmibs.so.40.0.0 \
    usr/lib/libnetsnmptrapd.so.40.0.0 usr/lib32/libnetsnmptrapd.so.40.0.0 ; do
    cp -a "$FACTORY/$rel" "$R/$rel"
done

# 4.2 net-snmp .40 版本符号链接
for d in usr/lib usr/lib32; do
    for lib in libnetsnmp libnetsnmpagent libnetsnmphelpers libnetsnmpmibs libnetsnmptrapd; do
        ( cd "$R/$d" && ln -sfn "$lib.so.40.0.0" "$lib.so.40" )
    done
done

# 4.3 顶层符号链接指向新版本（与出厂一致）
for d in usr/lib usr/lib32; do
    ( cd "$R/$d"
      ln -sfn libcurl.so.4.6.0 libcurl.so.4
      ln -sfn libssl.so.1.1    libssl.so
      ln -sfn libcrypto.so.1.1 libcrypto.so )
done

# 4.4 glib 2.54.2 -> 2.54.3
for d in usr/lib usr/lib32; do
    for lib in libglib-2.0 libgio-2.0 libgobject-2.0 libgmodule-2.0 libgthread-2.0; do
        if [ -e "$FACTORY/$d/$lib.so.0.5400.3" ]; then
            cp -a "$FACTORY/$d/$lib.so.0.5400.3" "$R/$d/"
            ( cd "$R/$d" && ln -sfn "$lib.so.0.5400.3" "$lib.so.0" )
        fi
    done
done

echo "=== 5. 批次二：消费者二进制 ==="
n=0
for d in usr/bin usr/sbin; do
    for f in $(ls "$FACTORY/$d"); do
        p="$FACTORY/$d/$f"
        [ -L "$p" ] && continue
        [ -f "$p" ] || continue
        if readelf -d "$p" 2>/dev/null | grep -qE 'libcrypto\.so\.1\.1|libssl\.so\.1\.1'; then
            cp -a "$p" "$R/$d/$f"; n=$((n+1))
        fi
    done
done
cp -a "$FACTORY/usr/libexec/ssh-sk-helper" "$R/usr/libexec/ssh-sk-helper"
ln -sfn snmpps "$R/usr/bin/snmptop"
echo "    复制 $n 个二进制"

echo "=== 6. 配置与附属文件 ==="
cp -a "$FACTORY/etc/init.d/S50nginx" "$R/etc/init.d/S50nginx"
for rel in etc/ssl/ct_log_list.cnf etc/ssl/ct_log_list.cnf.dist \
           etc/ssl/misc/tsget.pl etc/ssl/openssl.cnf.dist ; do
    [ -e "$FACTORY/$rel" ] && cp -a "$FACTORY/$rel" "$R/$rel"
done
cp -a "$FACTORY/usr/share/snmp/mibs/SNMP-USM-HMAC-SHA2-MIB.txt" "$R/usr/share/snmp/mibs/"
mkdir -p "$R/var/log/nginx"

echo "=== 7. 清理并打包 ==="
# init.d 必须是普通名字，rcS 在运行时会自己改成 IS_*
for s in S49ntp S50sshd S50is_web S50nginx; do
    [ -f "$R/etc/init.d/$s" ] || { echo "!! 缺少 $s"; exit 1; }
    [ -f "$R/etc/init.d/IS_$s" ] && { echo "!! 存在被污染的 IS_$s"; exit 1; }
done
rm -f "$R"/etc/ssh/ssh_host_*
rm -f "$R/usr/bin/qemu-arm-static"
rm -rf "$R/var/cache/nginx" "$R/run/nginx.pid" "$R/root/.rnd"
find "$R/dev" -maxdepth 1 -type c ! -name console -delete 2>/dev/null || true

cd "$R"
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "$OUT/rootfs.cpio.gz"
"$MKIMAGE" -A arm -O linux -T ramdisk -C none -a 0x88080000 -e 0x88080000 -n "" \
           -d "$OUT/rootfs.cpio.gz" "$OUT/rootfs.cpio.uboot"

echo
echo "=== 完成 ==="
"$MKIMAGE" -l "$OUT/rootfs.cpio.uboot"
echo "条目数 $(find "$R" | wc -l)"
sha256sum "$OUT/rootfs.cpio.uboot"
