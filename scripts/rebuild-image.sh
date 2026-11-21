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

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

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
# 3) nginx 路由：把 /restapi 与 /redfish 接到可用的 CGI 上
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
#
# 3) REST / Redfish 路由
#    出厂的 www/restapi 是个 CGI 脚本，但 uhttpd 默认只把 /cgi-bin 当 CGI，
#    放在 docroot 根目录的脚本只会被当静态文件下载 —— 所以 /restapi 从来
#    没通过。这里在 mgmtd 生成的 nginx.conf 里补两条 location，把标准路径
#    映射到已经能工作的 /cgi-bin/sweb/<api>。

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

# 把 /restapi 和 /redfish 接到已经可用的 /cgi-bin/sweb/<api> 上
enable_api_routes()
{
    conf=/etc/nginx/nginx.conf
    [ -f "${conf}" ] || return 0
    grep -q 'ubmc-api-routes' "${conf}" && return 0
    grep -q 'location.*/ws' "${conf}" || return 0

    tmp=/tmp/nginx.conf.api
    cp -a "${conf}" "${tmp}"
    awk '
        !done && index($0, "location") > 0 && index($0, "/ws") > 0 {
            print "        # ubmc-api-routes"
            print "        location ^~ /restapi {"
            print "            proxy_pass http://0.0.0.0:58080/cgi-bin/sweb/restapi;"
            print "        }"
            print "        location ^~ /redfish {"
            print "            proxy_pass http://0.0.0.0:58080/cgi-bin/sweb/redfish;"
            print "        }"
            done = 1
        }
        { print }
    ' "${tmp}" > "${conf}.new"

    if /usr/sbin/nginx -t -c "${conf}.new" >/dev/null 2>&1; then
        mv -f "${conf}.new" "${conf}"
        for pid in $(pidof nginx); do
            kill -HUP ${pid} 2>/dev/null
        done
    else
        rm -f "${conf}.new"
    fi
}

case "$1" in
    start)
        wait_mgmtd
        allow_hairpin vrf-pub 7681
        allow_open    vrf-pub 80
        ensure_luci_config
        enable_api_routes
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
echo "=== 6.6 REST / Redfish ==="
# 出厂的 www/restapi 是个 CGI 脚本，但 uhttpd 只把 /cgi-bin 当 CGI，放在
# docroot 根目录的脚本只会被当静态文件下载 —— 所以 /restapi 从来没通过。
# 这里的做法：把 Lua 实现搬过来，dispatcher 按路径首段分流，
# 再由 S46is_web_fw 在 nginx 里补 /restapi、/redfish 两条 location。
LR=opt/silc-web/host/usr/lib/lua/luci
mkdir -p "$R/$LR/restapi"
for f in restapi.lua redfish.lua http/protocol.lua \
         restapi/root.lua restapi/system.lua restapi/accounts.lua restapi/product.lua ; do
    cp -a "$FACTORY/$LR/$f" "$R/$LR/$f"
done

# 出厂把 vendor 判断注释掉了，customize() 会无条件清空整棵 REST 树，
# 结果每个 /restapi 请求都是 404。这里改成放行。
cat > "$R/$LR/restapi/product.lua" <<'RESTPROD'
module("luci.restapi.product",  package.seeall)

-- Upstream shipped this with the vendor guard commented out:
--
--   --if string.find(vendor, 'SILICOM') == nil then
--       for k, v in pairs(root_dict) do root_dict[k] = nil end
--       return
--   --end
--
-- so customize() wiped the whole REST tree unconditionally and every
-- /restapi request answered 404.  Keep the tree intact instead --
-- /etc/product/vendor is empty on the ATT-V150 anyway, so the original
-- "SILICOM only" test would have disabled the API here too.

function customize(root_dict)
end
RESTPROD

python3 - "$R/$LR/dispatcher.lua" <<'DISPATCH'
import sys
p = sys.argv[1]
s = open(p).read()

# A: 顶部 require
a = 'local nixio = require "nixio", require "nixio.util"\n'
assert s.count(a) == 1, ('A', s.count(a))
s = s.replace(a, a +
    '-- optional: a missing cjson/cpath must not take the whole web UI down\n'
    'local api_ok_redfish, redfish = pcall(require, "luci.redfish")\n'
    'local api_ok_restapi, restapi = pcall(require, "luci.restapi")\n', 1)

# B: 把 pam 认证拆出来，并加 HTTP Basic 认证
start = 'function authenticator.pam_htmlauth(validator, accs, default)'
endm = '\tluci.template.render("sysauth", {duser=default, fuser=user})\n\treturn false\n\nend\n'
i = s.index(start)
j = s.index(endm, i) + len(endm)
assert 'pam.authenticate' in s[i:j] and len(s[i:j]) < 1400
new = r'''-- base64 -> string, used by http_auth below
function from_base64(data)
	local b = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
	data = string.gsub(data, '[^'..b..'=]', '')
	return (data:gsub('.', function(x)
		if (x == '=') then return '' end
		local r,f='',(b:find(x)-1)
		for i=6,1,-1 do r=r..(f%2^i-f%2^(i-1)>0 and '1' or '0') end
		return r
	end):gsub('%d%d%d?%d?%d?%d?%d?%d?', function(x)
		if (#x ~= 8) then return '' end
		local c=0
		for i=1,8 do c=c+(x:sub(i,i)=='1' and 2^(8-i) or 0) end
		return string.char(c)
	end))
end

function pam_auth(user, pass)
	if user and pass then
		ret, pam_user, user_privil, proto = pam.authenticate(user, pass)
		if ret == "OK" then
			if user_privil == "null" then
				local path
				if proto == "radius" then
					path = "/config/radius/static/privilege"
				else
					path = "/config/unix/user/" .. pam_user.."/privilege"
				end
				ret, user_privil = mgmtdclient.query(path)
				if ret ~= "OK" then
					user_privil = "0"
				end
			end
			context.logname = user
			context.user_privil = tonumber(user_privil)
			context.auth_proto = proto
			return true
		end
	end
	return false
end

function authenticator.pam_htmlauth(validator, accs, default)
	local user = luci.http.formvalue("username")
	local pass = luci.http.formvalue("password")

	local auth_pass = pam_auth(user, pass)
	if auth_pass then
		if not util.contains(accs, user) then
			util.append(accs, user)
		end
		require "nixio"
		nixio.syslog("notice", "Web login as user " .. user)
		return user
	end

	require("luci.i18n")
	require("luci.template")
	context.path = {}
	luci.template.render("sysauth", {duser=default, fuser=user})
	return false

end

-- HTTP Basic auth for the programmatic APIs (/restapi, /redfish)
function http_auth(request)
	local auth = request:getenv("HTTP_AUTHORIZATION") or ""
	if auth ~= "" then
		local exist = string.find(auth, "^Basic %w+[=]?")
		if exist ~= nil then
			local token = string.gsub(auth, "Basic ", "")
			local userpasswd = from_base64(token)
			local user, pass
			for k, v in string.gmatch(userpasswd, "([%w_]+):([%w_]+)") do
				user = k
				pass = v
				break
			end
			if pam_auth(user, pass) then
				return true
			end
		end
	end
	luci.http.status(401, "Unauthorized")
	luci.http.header("WWW-Authenticate", "Basic realm='ubmc'")
	return false
end
'''
s = s[:i] + new + s[j:]

# C: httpdispatch 里按路径首段分流
old_c = '''	local stat, err = util.coxpcall(function()
		dispatch(context.request)
	end, error500)

	luci.http.close()
'''
assert s.count(old_c) == 1, ('C', s.count(old_c))
new_c = '''	-- programmatic APIs: /restapi/... and /redfish/...
	-- reached either directly (nginx maps /restapi -> /cgi-bin/sweb/restapi)
	-- or as /cgi-bin/sweb/restapi/...
	local api = r[1]
	if (api == "restapi" and api_ok_restapi) or
	   (api == "redfish" and api_ok_redfish) then
		if http_auth(request) then
			local uri = "/" .. table.concat(r, "/")
			local method = request:getenv("REQUEST_METHOD") or "GET"
			local stat, err = util.coxpcall(function()
				if api == "restapi" then
					restapi.handle_request(request, context, uri)
				elseif method == "GET" then
					redfish.handle_get(uri)
				elseif method == "POST" then
					redfish.handle_post(uri, http.formvalue(), http.content(), context)
				elseif method == "PATCH" then
					redfish.handle_patch(uri, http.formvalue(), http.content(), context)
				elseif method == "DELETE" then
					redfish.handle_delete(uri, http.formvalue(), http.content(), context)
				else
					redfish.handle_get(uri)
				end
			end, error500)
		end
		luci.http.close()
		return
	end

	local stat, err = util.coxpcall(function()
		dispatch(context.request)
	end, error500)

	luci.http.close()
'''
s = s.replace(old_c, new_c, 1)

open(p, 'w').write(s)
print("    dispatcher 已打补丁")
DISPATCH

echo "    REST/Redfish 已接入"

echo "=== 6.7 IPMI over LAN (RMCP+ / udp 623) ==="
IPMI_DIR="$SCRIPT_DIR/ipmi-lan"
HOSTBIN=/root/uBMC-fork/buildroot/output/host/bin
SYSROOT=/root/uBMC-fork/buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot

[ -f "$IPMI_DIR/ubmc_ipmi_lan.c" ] || { echo "!! 缺少 $IPMI_DIR/ubmc_ipmi_lan.c"; exit 1; }
[ -f "$IPMI_DIR/S47is_ipmi_lan" ]   || { echo "!! 缺少 $IPMI_DIR/S47is_ipmi_lan"; exit 1; }
[ -x "$HOSTBIN/arm-linux-gnueabihf-gcc" ] || { echo "!! 缺少交叉编译器"; exit 1; }

"$HOSTBIN/arm-linux-gnueabihf-gcc" --sysroot="$SYSROOT" -O2 -Wall \
    -o "$R/usr/sbin/ubmc_ipmi_lan" "$IPMI_DIR/ubmc_ipmi_lan.c" \
    -L"$SYSROOT/usr/lib" -lcrypto
"$HOSTBIN/arm-linux-gnueabihf-strip" "$R/usr/sbin/ubmc_ipmi_lan" 2>/dev/null || true
chmod 755 "$R/usr/sbin/ubmc_ipmi_lan"
echo "    ubmc_ipmi_lan $(stat -c%s "$R/usr/sbin/ubmc_ipmi_lan") 字节"

install -m 755 "$IPMI_DIR/S47is_ipmi_lan" "$R/etc/init.d/S47is_ipmi_lan"
echo "    /etc/init.d/S47is_ipmi_lan 已安装"

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
