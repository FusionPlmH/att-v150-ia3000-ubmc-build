# 自编译版当前还缺什么（2026-11-21 更新）

基线：公开源码 `bd9de8c` 自编译 + 已移植批次一。
对比对象：出厂 eMMC p2 的 `UBMC_1.2.0.8_BUILD_1d22f81a`。

```
diff -rq 统计
  只在原版:  89 项（原 110，已移植 21）
  只在我们:  13 项
  内容不同: 3045 项（其中 2378 项只差 build-id / pyc 时间戳）
```

---

## 一、真正影响功能的缺失

### ① OpenSSL 1.1.1f（`libcrypto.so.1.1` / `libssl.so.1.1`）

| | 我方 | 原版 |
|---|---|---|
| 版本 | **OpenSSL 1.0.2n** (2017-12) | **OpenSSL 1.1.1f** (2020-03) |

原版里链 1.1.1f 的二进制（共 35 个）：

```
ssh, scp, sftp, sshd, ssh-add, ssh-agent, ssh-keygen, ssh-keyscan
snmpd, snmptrapd, snmpget, snmpgetnext, snmpwalk, snmpbulkget, snmpbulkwalk
snmpset, snmptrap, snmpdf, snmpstatus, snmptable, snmptest, snmptranslate
snmpusm, snmpvacm, snmpdelta, snmpnetstat, snmpps, snmpping, snmppcap
snmptls, agentxtrap, encode_keychange
curl, openssl, nginx
```

我方这 35 个全部链 `libcrypto.so.1.0.0`。

**影响**：安全补丁级别、TLS 1.3 支持、部分新算法。功能上不阻塞（我方自洽能跑）。

### ② net-snmp 5.9（`libnetsnmp*.so.40`）—— **有实质功能差距**

| | 我方 5.7.3 | 原版 5.9 |
|---|---|---|
| USM 认证协议 | `usmHMACMD5AuthProtocol`<br>`usmHMACSHA1AuthProtocol` | **MD5 / SHA / SHA-224 / SHA-256 / SHA-384 / SHA-512** |

原版 `libnetsnmp.so.40` 里的原文：

```
-a PROTOCOL   set authentication protocol (MD5|SHA|SHA-224|SHA-256|SHA-384|SHA-512)
```

**这是我方唯一"用户能直接感知到"的功能缺失** ——
如果 SNMPv3 要用 SHA-2 认证（现在普遍要求，SHA-1 已被判不安全），我方做不到。

### ③ REST API / Redfish（7 项）

```
opt/silc-web/host/usr/lib/lua/luci/restapi.lua      346 行
opt/silc-web/host/usr/lib/lua/luci/redfish.lua      488 行
opt/silc-web/host/usr/lib/lua/luci/restapi/         4 个文件 471 行
opt/silc-web/host/www/restapi/                      1 个文件
（+ /opt/silc-web/host/luci/ 下两条软链）
```

**影响**：只有程序化 API 访问。Web UI、IPMI、SNMP、CLI、RPC 全都不经过它。
详见 [web-stack.md](web-stack.md)。

**不能只拷文件**：`dispatcher.lua` 要加分派，还依赖 `http_auth()`/`uri`/`context`
等自编译版 dispatcher 没有的辅助函数。

### ④ 依赖新库的 5 个工具

```
usr/bin/snmpping      需 libnetsnmp.so.40 + libssl.so.1.1
usr/bin/snmpps        需 libnetsnmp.so.40 + libncurses.so.6 + libssl.so.1.1
usr/bin/snmptop       需 libnetsnmp.so.40 + libncurses.so.6 + libssl.so.1.1
usr/bin/snmppcap      需 libnetsnmp.so.40 + libpcap.so.1 + libssl.so.1.1
usr/libexec/ssh-sk-helper   需 libcrypto.so.1.1 + libssl.so.1.1
```

跟着 ①② 一起解决。

### ⑤ glib 2.54.2 → 2.54.3

```
我方  /usr/lib/libglib-2.0.so.0.5400.2
原版  /usr/lib/libglib-2.0.so.0.5400.3
```
补丁级差异，`libgio`/`libgobject`/`libgmodule`/`libgthread` 同样。
功能上无影响。

---

## 二、不影响本机的缺失

### ⑥ `bmc-fan.lua` —— **只对 ATT-V250 / ATT-V450 注册**

原版 `controller/admin/bmc.lua`：

```lua
if oem_model == 'ATT-V250' or oem_model == 'ATT-V450' then
    entry({"admin", "bmc", "fan"}, cbi("admin/bmc-fan"), _("Thermal"), 4)
end
```

**本机是 V150，这个分支根本不会执行。** 缺了它 Web UI 也不会少任何菜单。

配套的 `etc/product/UBMC/defconfig/hw_config/thermal_profile/fullspeed.conf`
同理 —— 只有那个 Thermal 页面读它。

### ⑦ CREANORD 厂商定制（6 项）

```
etc/product/UBMC/defconfig/vendor_config/CREANORD/
opt/silc-web/host/etc/config/CREANORD.luci
opt/silc-web/host/etc/uci-defaults/luci-theme-creanord
opt/silc-web/host/luci/view/themes/creanord/
opt/silc-web/host/usr/lib/lua/luci/view/themes/creanord/
opt/silc-web/host/www/luci-static/creanord/
```

本机 `Vendor: ATT`，不涉及。

### ⑧ phonehome / Keysight（2 项）

```
etc/phonehome          电话回家脚本
etc/init.d/S99seth     Keysight 专用启动脚本
```

第三方 OEM 功能，本机用不到。

### ⑨ openssl 附属文件（3 项）

```
etc/ssl/ct_log_list.cnf       证书透明度日志列表（curl 用，可选）
etc/ssl/openssl.cnf.dist      配置模板备份
etc/ssl/misc/tsget.pl         时间戳工具脚本
```

---

## 三、原版里其实是「死文件」

这几个很容易被误当成缺失，实际**无人引用**（已用 `readelf -d` 全树扫描确认）：

| 文件 | 状态 |
|---|---|
| `libcurl.so.4.6.0`（+lib32）| **零引用** —— 我方 `libcurl.so.4.5.0` 才是活的 |
| `libssh.so.4.8.6`（+lib32）| **零引用** —— 我方 `libssh.so.4.4.2` 才是活的 |
| `libnetsnmp*.so.35*`（13 个，+lib32）| 只被**彼此**引用，没有可执行文件用它 |
| `usr/lib/tmpfiles.d/sudo.conf` | systemd 专用，我方是 busybox init |
| `usr/lib64/sa/{sa1,sa2,sadc}` | sysstat 的重复路径，我方在 `/usr/lib/sa` |
| `usr/share/snmp/mibs/SNMP-USM-HMAC-SHA2-MIB.txt` | 纯 MIB 文档 |

> 注：原版 net-snmp 的 `.35` 序列（5.8）是中间产物，实际用户态全部用 `.40`（5.9）。
> 这说明厂家自己也是逐版往上加、老版本没清。

## 四、内核模块的 1 处差异

```
lib/modules/4.14.54/kernel/drivers/i2c/busses/ubmc-ipmi-slave.ko
    原版 51,112 B   我方 51,240 B   (+128 B)
```

这是 IPMI-over-i2c（SSIF）从机驱动。316 个模块里**只有这一个**不同。

---

## 五、汇总

| 优先级 | 项目 | 影响 | 能否只拷文件 |
|---|---|---|---|
| 🔴 高 | net-snmp 5.9（SHA-2 认证）| **功能缺失** | ❌ 需重编 |
| 🟡 中 | OpenSSL 1.1.1f | 安全/TLS 版本 | ❌ 需重编 |
| 🟡 中 | REST API / Redfish | 程序化 API | ⚠️ 需改 dispatcher |
| 🟢 低 | glib 2.54.3 | 无感知 | ❌ 需重编 |
| ⚪ 不需要 | bmc-fan.lua、hw_config | **V150 不用** | — |
| ⚪ 不需要 | CREANORD、phonehome、S99seth | 厂商专用 | — |
| ⚪ 不需要 | libcurl 4.6.0、libssh 4.8.6、netsnmp .35 | **死文件** | — |

### 已补齐的（批次一，从原版移植）

valgrind 全套（10 个二进制 + 116×2 平台文件）、`ntpq`、`ssh-copy-id`、
`json2lua`/`lua2json`、`cjson.so`（+lib32）、`/usr/share/lua/5.1/*`。
依赖体检零缺失。
