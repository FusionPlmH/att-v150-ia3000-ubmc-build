# 还缺什么（2026-11-21 第三轮后，实测数据）

基线：`ubmc-bank1/rootfs.cpio.uboot` 第三轮版
（54,311,469 B / `5b26f335…`），对比对象是出厂 1.2.0.8。

结论先行：

- **库层面零缺失** —— 1,191 个 ELF 扫下来，未解析的 NEEDED **0 种**
- **真正的功能缺口只剩两个**：REST API / Redfish、IPMI over LAN
- 其余 39 项全是厂商专用、死文件或冗余路径

## 一、接口层面（Web/CLI 的后端能力）

### mgmtd 插件：完全一致

```
/usr/lib/libprod_mgmtd_inst.so.0.0.0
  原厂 73 条  /  我们 73 条   comm 双向为空
```

覆盖 `/action/bmc/{power,usb-cdrom,bios,sel,images,phonehome}`、
`/status/bmc/{power,usb-cdrom,console,health,info}`。

**注意**：`is_mgmtd` 本体里一条 `/action/bmc/*` 都没有，两版都是 0 命中。
找 BMC 能力不要只看 `is_mgmtd`。

### is_mgmtd 本体：479 vs 481

| 差异 | 端点 | 影响 |
|---|---|---|
| 只有原厂有 | `/action/system/check-upgrade-permit` | **无**。实测全树没有任何 Lua/脚本调用它 |
| 只有原厂有 | `/config/extend/%s` | 无。我们已有 `/config/extend` 节点，`/%s` 只是模板串 |
| 只有原厂有 | `/config/system/misc/log/template` | 无。配置树里没有这个键 |
| 只有我们有 | `/action/system/get-config-list` | 我们是多出来的 |

即 **1.1.0.5 与 1.2.0.8 在动作面上没有实际可用性差异**。

## 二、文件层面：缺 46 项，逐条定性

全量清单对比：原厂 7,368 / 我们 7,331，缺 46 项。

### 🟡 A. REST API / Redfish —— 唯一真正的功能缺口（11 个文件）

```
opt/silc-web/host/usr/lib/lua/luci/restapi.lua          346 行
opt/silc-web/host/usr/lib/lua/luci/redfish.lua          488 行
opt/silc-web/host/usr/lib/lua/luci/restapi/accounts.lua
opt/silc-web/host/usr/lib/lua/luci/restapi/product.lua
opt/silc-web/host/usr/lib/lua/luci/restapi/root.lua
opt/silc-web/host/usr/lib/lua/luci/restapi/system.lua
opt/silc-web/host/www/restapi                            （软链 + 静态资源）
```

**不能只拷文件**：`dispatcher.lua` 要加 `/restapi` 与 `/redfish` 分派，
而这两个 Lua 依赖 `http_auth()` / `uri` / `context` 等自编译版 dispatcher
里没有的辅助函数。

影响面：只影响**程序化 API**。Web UI、Console、Power、USB CDROM、IPMI(KCS)、
SNMP、CLI、JSON-RPC 全都不经过它。

现状实测（本机）：

```
https://<bmc>/restapi/v1/   -> 404
https://<bmc>/redfish/v1/   -> 404
https://<bmc>/rpc/sys       -> 可用（且能被提权成 root）
```

### ⚪ B. 厂商专用（不影响本机，22 个文件）

```
CREANORD 主题/配置/MIB/证书           17 项
  opt/silc-web/host/www/luci-static/creanord/…
  opt/silc-web/host/etc/config/CREANORD.luci
  etc/product/UBMC/defconfig/vendor_config/CREANORD/…
phonehome（Keysight OEM）+ S99seth      3 项
bmc-fan.lua + thermal_profile/fullspeed.conf  2 项
```

本机 `Vendor: ATT`，`oem_model` 是 V150：

- CREANORD 是另一家 OEM 的皮肤，ATT 配置里不引用
- `bmc-fan.lua` 只在 `oem_model` 为 `ATT-V250/V450` 时注册
- `fullspeed.conf` 同理
- phonehome/S99seth 是 Keysight 的回报模块

### ⚪ C. 死文件与冗余路径（12 个文件）

```
usr/lib/libnetsnmp*.so.35(.0.0)      10 项   只被彼此引用的孤儿
usr/lib/tmpfiles.d/sudo.conf          1 项   systemd 专用，我们是 busybox init
usr/lib64/sa/sa{1,2}, sadc            3 项   与 usr/lib/sa/ 重复（我们已有）
```

`.35` 系列已在 [dead-libs-audit.md](./dead-libs-audit.md) 逐条核查过：
出厂镜像里就是孤儿，没有任何 ELF 引用它们。

## 三、库层面：零缺失（实测）

对 v4 镜像全树扫描：

```
扫描 ELF: 1191 个
未解析的 NEEDED: 0 种
```

批次二已从出厂移植（同 SoC、同内核 4.14.54，ABI 兼容）：

| 组件 | 原基线 | 现在 |
|---|---|---|
| OpenSSL | 1.0.2n | **1.1.1f**（TLS 1.3 可用）|
| net-snmp | 5.7.3 | **5.9**（SHA-224/256/384/512）|
| OpenSSH | 7.6p1 | **8.2p1** |
| nginx | 1.12.2 | **1.20.2** |
| curl / libcurl | 7.58.0 / 4.5.0 | **7.68.0 / 4.6.0** |
| libssh | 0.7.5 / 4.4.2 | **1.6.0 / 4.8.6** |
| glib | 2.54.2 | **2.54.3** |

批次一补的工具：valgrind 全套、`ntpq`、`ssh-copy-id`、`json2lua`/`lua2json`、
`cjson.so`、SNMP SHA-2 MIB。

## 四、新发现的真缺口：IPMI over LAN

这不是「没移植」，而是**固件里根本没有**：

```
ss -uln   -> 只有 0.0.0.0:68（DHCP client），没有 623/udp
也没有 ipmitool 二进制
```

现有 IPMI 是 **KCS-only**：`ubmc_ipmi` + `ubmc_ipmi_slave.ko`，
只在主机 OS 内部可用。要支持 `ipmitool -I lanplus -H <bmc>`，需要
自己实现 IPMI 2.0 RMCP+ 服务端（UDP 623、会话、认证、加密），
再把 NetFn Chassis/Sensor/SEL/FRU 映射到现有的
`ubmc_sys_ctrl.sh` 与 `/status/bmc/*`。

**但用户列的需求（看输出、发按键、开关机重启、挂 ISO）都不依赖它** ——
Web UI 已经全覆盖，见 [ipmi-parity.md](./ipmi-parity.md)。

## 五、组件版本陈旧（不是缺，但要知道）

ABI 限制了上限：内核 4.14.54、glibc 2.27、Buildroot 2018.02（2019 EOL），
所以这些都没法再往上推，除非整体重做工具链。

| 组件 | 版本 | 状态 |
|---|---|---|
| Buildroot | 2018.02 | 2019-03 EOL |
| Python | 2.7.14 | 2020-01 EOL（mgmtd 的 pyc 依赖它）|
| OpenSSL | 1.1.1f | 2023-09 EOL（1.1.1 分支）|
| OpenSSH | 8.2p1 | 2020 年版本，之后有 CVE |
| curl | 7.68.0 | 2020 年版本，之后有多个 CVE |
| glib | 2.54.3 | 很旧 |
| net-snmp | 5.9 | 尚可用 |

出厂固件自己也是这个水平 —— 它甚至同时留了
`libcrypto.so.1.0.0` 和 `libcrypto.so.1.1`，`ntpq` 还挂在 1.0.2 上。
所以这不是我们移植引入的倒退。

## 六、汇总

| 类别 | 项数 | 结论 |
|---|---|---|
| 库（NEEDED） | 0 | ✅ 全解析 |
| 接口（插件） | 0 | ✅ 73 = 73 |
| 接口（mgmtd 本体） | 3 | ⚪ 无人调用，无影响 |
| REST API / Redfish | 11 文件 | 🟡 **真缺口**，仅影响程序化 API |
| IPMI over LAN | — | 🟡 **真缺口**，需自己实现 |
| 厂商专用（CREANORD/phonehome/bmc-fan） | 22 文件 | ⚪ 本机用不到 |
| 死文件 / 冗余 | 12 文件 | ⚪ 无引用 |

## 七、还不算「缺」但可以做的优化

1. **REST/Redfish 移植** —— 主要工作在 `dispatcher.lua`。做完就有
   `/restapi/v1/*` 和 `/redfish/v1/*`，可以用 Redfish 客户端管理。
2. **IPMI over LAN** —— 独立工程量，但做出来后 `ipmitool` 生态全通。
3. **`S46is_web_fw` 上游化** —— 如果能改 mgmtd 源码，把 7681/80 的防火墙
   例外交给它自己的模板生成，init 脚本就可以去掉。
4. **杀掉 `i` 系列冗余** —— 出厂遗留的 .35 库和 `usr/lib64/sa` 可以清掉，
   省 ~6 MB，但收益不大。
