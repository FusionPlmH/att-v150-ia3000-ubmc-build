# 自编译版当前还缺什么（2026-11-21 第二轮后更新）

基线：公开源码 `bd9de8c` 自编译 + 批次一 + 批次二。

```
diff -rq 统计
  只在原版:  41 项（原 89 → 已补 48）
  只在我们:  13 项
  内容不同: 3045 项（其中 2378 项只差 build-id / pyc 时间戳）
```

## 已补齐

### 批次一（纯新增，零风险）

valgrind 全套（10 个二进制 + 116×2 平台文件）、`ntpq`、`ssh-copy-id`、
`json2lua`/`lua2json`、`cjson.so`（+lib32）、`/usr/share/lua/5.1/*`。

### 批次二（库与工具升级）

| 组件 | 原来 | 现在 | 来源 |
|---|---|---|---|
| OpenSSL | 1.0.2n | **1.1.1f** | 出厂固件 |
| net-snmp | 5.7.3 | **5.9** | 出厂固件 |
| OpenSSH | 7.6p1 | **8.2p1** | 出厂固件 |
| nginx | 1.12.2 | **1.20.2** | 出厂固件 |
| curl / libcurl | 7.58.0 / 4.5.0 | **7.68.0 / 4.6.0** | 出厂固件 |
| libssh | 0.7.5 / 4.4.2 | **1.6.0 / 4.8.6** | 出厂固件 |
| glib | 2.54.2 | **2.54.3** | 出厂固件 |

方式：出厂固件与本机同 SoC、同内核（4.14.54），二进制与库 ABI 完全兼容，
直接移植即可，不需要重编 buildroot。

qemu-arm-static 实测：

```
openssl    OpenSSL 1.1.1f  31 Mar 2020
curl       curl 7.68.0  libcurl/7.68.0  OpenSSL/1.1.1f
ssh        OpenSSH_8.2p1, OpenSSL 1.1.1f
snmpwalk   NET-SNMP version: 5.9
           -a PROTOCOL (MD5|SHA|SHA-224|SHA-256|SHA-384|SHA-512)
nginx      nginx/1.20.2          nginx -t: test is successful
sshd       OpenSSH_8.2p1         sshd -t : 通过
rrdtool    RRDtool 1.6.0         （glib 升级后正常）
tpm2-abrmd version 1.2.0         （glib 升级后正常）
TLS 1.3    TLS_AES_256_GCM_SHA384  TLSv1.3
```

---

## 仍然缺少的 41 项

### 🟡 唯一真正的功能缺口：REST API / Redfish（7 项）

```
opt/silc-web/host/usr/lib/lua/luci/restapi.lua      346 行
opt/silc-web/host/usr/lib/lua/luci/redfish.lua      488 行
opt/silc-web/host/usr/lib/lua/luci/restapi/         4 个文件
opt/silc-web/host/www/restapi/
（+ 两条软链）
```

**不能只拷文件**：`dispatcher.lua` 要加分派，还依赖 `http_auth()`/`uri`/`context`
等自编译版 dispatcher 没有的辅助函数。只影响程序化 API ——
Web UI、IPMI、SNMP、CLI、RPC 全都不经过它。

### ⚪ 无影响（34 项）

| 项 | 数量 | 原因 |
|---|---|---|
| `libnetsnmp*.so.35*` | 20 | 原版里的**死文件**，只被彼此引用 |
| CREANORD 主题/配置 | 8 | 本机 `Vendor: ATT` |
| `phonehome`、`S99seth` | 2 | Keysight/CREANORD OEM |
| `bmc-fan.lua`、`hw_config` | 2 | 只在 `oem_model` 为 ATT-V250/V450 时注册 |
| `tmpfiles.d/sudo.conf`、`usr/lib64/sa` | 2 | systemd 专用 / 重复路径 |

---

## 五、汇总

| 状态 | 项目 | 备注 |
|---|---|---|
| ✅ 已补（批次二）| net-snmp 5.9 —— SHA-224/256/384/512 | 移植出厂二进制，零重编 |
| ✅ 已补（批次二）| OpenSSL 1.1.1f + TLS 1.3 | 移植出厂库 |
| ✅ 已补（批次二）| OpenSSH 8.2p1、nginx 1.20.2、curl 7.68.0、libssh 1.6.0 | 同上传 |
| ✅ 已补（批次二）| glib 2.54.3、snmptop、openssl 附属文件、SNMP SHA-2 MIB | — |
| ✅ 已补（批次一）| valgrind 全套、ntpq、ssh-copy-id、json2lua/lua2json、cjson | — |
| 🟡 仍缺 | REST API / Redfish（7 项）| 需改 dispatcher，仅影响程序化 API |
| ⚪ 不需要 | CREANORD、phonehome、S99seth、bmc-fan、hw_config | 厂商专用 / V150 不用 |
| ⚪ 不需要 | netsnmp .35 系列、tmpfiles.d、usr/lib64 | 死文件 / 冗余 |

### 已补齐的（批次一，从原版移植）

valgrind 全套（10 个二进制 + 116×2 平台文件）、`ntpq`、`ssh-copy-id`、
`json2lua`/`lua2json`、`cjson.so`（+lib32）、`/usr/share/lua/5.1/*`。
依赖体检零缺失。
