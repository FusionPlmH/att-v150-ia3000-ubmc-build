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

echo "=== 6.5 nginx 转发链与 Web UI 补丁 ==="
# 1) 防火墙：vrf-pub 的兜底 DROP 缺 7681（ttyd）和 80（301 跳转）的例外
# 2) /etc/config/luci：缺失时所有 /admin 页面都会 Authorization Required
cat > "$R/etc/init.d/S46is_web_fw" <<'WEBFW'
#!/bin/sh
#
# nginx 转发链与 Web UI 所需的补丁。
#
# 1) 防火墙例外
#    ATT 的默认配置在 vrf-pub 上有一条兜底规则：
#        -I INPUT -i vrf-pub -j DROP
#    is_mgmtd 建完这条规则后，只用它自己那个模板给后端端口 uhttpd:58080
#    补例外，其余 nginx 相关端口全漏了：
#
#      7681  ttyd —— nginx 的 /ws 上游（Web Console Shell）。
#            nginx 从 vrf-pub 出去再绕回来（hairpin），源 MAC 是 vrf-pub
#            自己的 MAC。缺例外 → /ws 永远握手不上，Console 页面一直转圈。
#
#      80    nginx 的 HTTP 端口，只做 301 跳转到 https://。
#            缺例外 → 浏览器打开 http://<bmc>/ 一直转圈而不跳转。
#
#    7681 沿用 58080 的做法：只放行「源 MAC = vrf-pub 自身 MAC」的流量，
#    局域网直连 7681 仍然 DROP。80 只回一个 301，允许外部访问。
#
# 2) LuCI 会话配置
#    LuCI 把会话目录写在 /etc/config/luci 的 sauth.sessionpath 里，这个文件
#    本应由 is_mgmtd 从 /opt/silc-web/host/etc/config/luci 复制过去。实测
#    开机后可能不存在 —— 一旦缺失，登录能过但所有 /admin 页面都会返回
#    "Authorization Required"，整个 Web UI（含 Console/Power/USB）全废。

wait_mgmtd()
{
    i=0
    while [ ! -e /tmp/mgmtd_started ] && [ $i -lt 120 ]; do
        sleep 1
        i=$((i + 1))
    done
}

# "-i <vrf> -j DROP" 兜底规则在链里的位置（1 起算），没有则 0
drop_pos()
{
    vrf="$1"
    lineno=$(iptables -S INPUT | grep -n -- "-i ${vrf} -j DROP" |
             head -1 | cut -d: -f1)
    [ -n "${lineno}" ] && echo $((lineno - 1)) || echo 0
}

# 插到兜底 DROP 之前；位置按当前链算，不依赖脚本相对 mgmtd 的执行时机
insert_accept()
{
    vrf="$1"
    port="$2"
    if ! iptables -C INPUT -i ${vrf} -p tcp --dport ${port} -j ACCEPT 2>/dev/null; then
        pos=$(drop_pos ${vrf})
        [ "${pos}" -gt 1 ] || pos=2
        iptables -I INPUT ${pos} -i ${vrf} -p tcp --dport ${port} -j ACCEPT
    fi
}

# 只允许 nginx 的 hairpin（源 MAC = VRF 设备自身 MAC）
allow_hairpin()
{
    vrf="$1"
    port="$2"
    dev=/sys/class/net/${vrf}
    [ -e "${dev}" ] || return 0
    mac=$(cat ${dev}/address)

    if ! iptables -C INPUT -i ${vrf} -p tcp --dport ${port} \
        -m mac ! --mac-source ${mac} -j DROP 2>/dev/null; then
        iptables -I INPUT 1 -i ${vrf} -p tcp --dport ${port} \
            -m mac ! --mac-source ${mac} -j DROP
    fi
    insert_accept ${vrf} ${port}
}

allow_open()
{
    [ -e /sys/class/net/$1 ] || return 0
    insert_accept "$1" "$2"
}

ensure_luci_config()
{
    tpl=/opt/silc-web/host/etc/config/luci
    [ -f "${tpl}" ] || return 0
    if [ ! -f /etc/config/luci ]; then
        mkdir -p /etc/config
        cp -a "${tpl}" /etc/config/luci
    fi
    spp=$(sed -n 's/^[ 	]*option[ 	]*sessionpath[ 	]*//p' /etc/config/luci |
          head -1)
    if [ -n "${spp}" ]; then
        mkdir -p "${spp}"
        chmod 700 "${spp}"
    fi
}

case "$1" in
    start)
        wait_mgmtd
        allow_hairpin vrf-pub 7681
        allow_open    vrf-pub 80
        ensure_luci_config
        ;;
    stop)
        vrf=vrf-pub
        if [ -e /sys/class/net/${vrf} ]; then
            mac=$(cat /sys/class/net/${vrf}/address)
            iptables -D INPUT -i ${vrf} -p tcp --dport 7681 -j ACCEPT 2>/dev/null
            iptables -D INPUT -i ${vrf} -p tcp --dport 7681 \
                -m mac ! --mac-source ${mac} -j DROP 2>/dev/null
            iptables -D INPUT -i ${vrf} -p tcp --dport 80 -j ACCEPT 2>/dev/null
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop}"
        exit 1
        ;;
esac

WEBFW
chmod 755 "$R/etc/init.d/S46is_web_fw"
sh -n "$R/etc/init.d/S46is_web_fw"
echo "    已安装 S46is_web_fw"
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
