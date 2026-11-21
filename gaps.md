# 还缺什么（2026-11-21 第七轮后，实测数据）

基线：`ubmc-bank1/rootfs.cpio.uboot` 第七轮版
（54,340,651 B / `650ec5a1…`），对比对象是出厂 1.2.0.8。

结论先行：

- **库层面零缺失** —— 1,191 个 ELF 扫下来，未解析的 NEEDED **0 种**
- **REST API / Redfish 已补齐**（第四轮，本机实测 200）
- **IPMI over LAN 已补齐**（第五~七轮）：RMCP+ 会话、电源控制、SEL、SDR/传感器、
  FRU、bootdev、SOL（含 BREAK 与 ACK 流控）全部实测通过，pyghmi 与 python-ipmi
  两个独立客户端交叉验证
- 真正的功能缺口已经清零，其余 39 项全是厂商专用、死文件或冗余路径

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

### ✅ A. REST API / Redfish —— 第四轮已补齐

原先缺 11 个文件，现在从出厂移植并接通：

```
opt/silc-web/host/usr/lib/lua/luci/restapi.lua          346 行
opt/silc-web/host/usr/lib/lua/luci/redfish.lua          488 行
opt/silc-web/host/usr/lib/lua/luci/restapi/{root,system,accounts,product}.lua
opt/silc-web/host/usr/lib/lua/luci/http/protocol.lua    换出厂版
opt/silc-web/host/usr/lib/lua/luci/dispatcher.lua       加 http_auth + 分流
```

**踩到两个坑**：

1. **路由从来没接上** —— 出厂的 `www/restapi` 是个 CGI 脚本，但 uhttpd
   默认只把 `/cgi-bin` 当 CGI。实测在 docroot 根目录放可执行脚本，
   请求它会**把源码当静态文件吐出来**，不会执行。所以 `/restapi` 一直是死的。
2. **上游自己把 REST 关了** —— `restapi/product.lua` 里 vendor 判断被注释掉：

   ```lua
   --if string.find(vendor, 'SILICOM') == nil then
       for k, v in pairs(root_dict) do root_dict[k] = nil end
       return
   --end
   ```

   `customize()` 于是无条件清空整棵 REST 树，每个请求都是 404。

**做法**：`dispatcher.lua` 按路径首段分流（比原厂按 `REQUEST_URI` 前缀更稳），
`S46is_web_fw` 在 mgmtd 生成的 nginx.conf 里补两条 location，
`product.lua` 改成放行，可选依赖用 `pcall` 包起来（缺 `cjson` 也不会
拖垮整个 Web UI）。

**实测**：

```
GET /restapi/v1/config/system   无认证 -> 401
GET /restapi/v1/config/system   认证后 -> 200（完整系统配置 JSON）
GET /restapi/v1/status/system   认证后 -> 200（会话列表）
GET /restapi/v1                 认证后 -> 403（顶层不允许）
GET /redfish/v1                 认证后 -> 200（标准 ServiceRoot）
GET /redfish/v1/Manager         认证后 -> 200（含真实时间）
GET /redfish/v1/Chassis/Members/0/Power -> 200
```

认证用 HTTP Basic + PAM 账号（`is_admin` / `51L1C0M201B`）。

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

## 四、IPMI over LAN（第五~七轮已补齐）

原状态：**固件里根本没有**，不是「没移植」。

```
ss -uln   -> 只有 0.0.0.0:68（DHCP client），没有 623/udp
也没有 ipmitool 二进制
```

现有 IPMI 是 **KCS-only**：`ubmc_ipmi` + `ubmc_ipmi_slave.ko`，只在主机 OS 内部可用。

自己实现了 IPMI 2.0 RMCP+ 服务端（`ubmc_ipmi_lan`，cipher suite 3）：

| 能力 | 命令 | 实测客户端 |
|---|---|---|
| 会话 / 设备信息 | App 0x01/0x38/0x3B/0x3C/0x3D/0x42 | pyghmi、python-ipmi |
| 电源控制 | Chassis 0x02/0x03 + 0x00/0x01 | pyghmi（`get_power` → `on`）|
| SEL | Storage 0x40/0x42/0x43/0x47/0x48/0x49 | pyghmi 199 条、python-ipmi 2189 行 |
| SDR / 传感器 | Storage 0x20/0x21/0x22/0x23 + Sensor 0x2D/0x23 | pyghmi 24 个有值、python-ipmi 逐个列全 |
| FRU | Storage 0x10/0x11 | pyghmi `get_inventory`、python-ipmi `fru print` |
| boot options | Chassis 0x08/0x09 | pyghmi 读写往返 |
| SOL | App 0x48/0x49/0x4A/0x4B + payload type 1 | pyghmi（含 BREAK、ACK 流控）|
| ASF presence ping | RMCP class 6 | python-ipmi（握手前置）|

细节与踩坑见 [ipmi-lan.md](./ipmi-lan.md) 和 [sol-design.md](./sol-design.md)。

仍缺（都不影响日常运维）：

- **LAN configuration parameters**（Transport 0x0C 0x02）—— `ipmitool lan print` 能跑通，
  但 IP/掩码/网关会显示 `not supported`。要做需要 `recvmsg` + `IP_PKTINFO` 拿目的地址。
- **SHA-256 cipher suite（17/18/19）** —— python-ipmi 默认挑 17，会被拒；加 `-o cipher=3` 即可。
- **主机 → 客户端的 BREAK 通知**（`payload[3] & 0x04`）—— console socket 把线路事件抹掉了。
- Set LAN Config / Get-Set User Access / IPMB 桥接 / PEF / SDR 写入。

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
| REST API / Redfish | 0 | ✅ **第四轮补齐并实测** |
| IPMI over LAN | 0 | ✅ **第五~七轮补齐并实测**（含 SEL/SDR/FRU/SOL/BREAK/流控）|
| 厂商专用（CREANORD/phonehome/bmc-fan） | 22 文件 | ⚪ 本机用不到 |
| 死文件 / 冗余 | 12 文件 | ⚪ 无引用 |

## 七、还不算「缺」但可以做的优化

1. ~~**REST/Redfish 移植**~~ —— 已完成。
2. ~~**IPMI over LAN**~~ —— 已完成（`ubmc_ipmi_lan` + `S47is_ipmi_lan`）。
   继续往深做的话，下一个是 SEL / SDR 传感器桥接。
3. **`S46is_web_fw` 上游化** —— 如果能改 mgmtd 源码，把 7681/80 的防火墙
   例外和 restapi/redfish 路由都交给它自己的模板生成，init 脚本就可以去掉。
4. **杀掉冗余** —— 出厂遗留的 .35 库和 `usr/lib64/sa` 可以清掉，
   省 ~6 MB，但收益不大。
