# 还缺什么（2026-11-22 第二十五轮后，实测数据）

基线：`ubmc-bank1/rootfs.cpio.uboot` 第二十五轮版
（55,823,961 B / `2e5efcec…`），对比对象是出厂 1.2.0.8。

结论先行：

- **库层面零缺失** —— 1,191 个 ELF 扫下来，未解析的 NEEDED **0 种**
- **REST API / Redfish 已补齐**（第四轮，本机实测 200 / 未认证 401）
- **IPMI over LAN 已补齐**（第五~七轮）：RMCP+ 会话、电源控制、SEL、SDR/传感器、
  FRU、bootdev、SOL（含 BREAK 与 ACK 流控）全部实测通过，pyghmi 与 python-ipmi
  两个独立客户端交叉验证
- **第二十五轮之后，IPMI 侧只剩一项「不做」**：IPMB 桥接（板上没有可桥对象，
  `0x2C` 回 `0xC1` 就是标准应答）。用户管理、Set LAN 全参数、PEF + PET/syslog
  发送器、SDR 仓库写入都已补齐
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

### ✅ A. REST API / Redfish —— 第四轮已补齐（第二十五轮实测 `/restapi/v1/status/system` → 200）

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

认证用 HTTP Basic + PAM 账号（`is_admin` / 它的 Web 口令）。

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

### ⚪ C. 死文件与冗余路径（12 个文件）—— **我们的镜像里本来就没有**

```
usr/lib/libnetsnmp*.so.35(.0.0)      10 项   只被彼此引用的孤儿
usr/lib/tmpfiles.d/sudo.conf          1 项   systemd 专用，我们是 busybox init
usr/lib64/sa/sa{1,2}, sadc            3 项   与 usr/lib/sa/ 重复（我们已有）
```

这 12 项只存在于**出厂** 1.2.0.8 镜像里；我们的基线是公开源码 bd9de8c 自己
buildroot 出来的，实测（v20 镜像 + 真机）都不存在：

```
/usr/lib/libnetsnmp*.so.35*  -> 0 个
/usr/lib64/sa                -> 不存在
/usr/lib/tmpfiles.d          -> 不存在
/usr/lib/sa/{sa1,sa2,sadc}   -> 存在（唯一一份，68 KB 的 sadc）
```

所以「清死文件」这条**无需动作**，收益（~6 MB）在重建时已经自然拿到。

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
| SEL | Storage 0x40/0x42/0x43/0x47/0x48/0x49 | pyghmi 约 190 条、python-ipmi 2189 行 |
| SDR / 传感器 | Storage 0x20/0x21/0x22/0x23 + Sensor 0x2D/0x23 | pyghmi 24 个有值、python-ipmi 逐个列全 |
| FRU | Storage 0x10/0x11 | pyghmi `get_inventory`、python-ipmi `fru print` |
| boot options | Chassis 0x08/0x09 | pyghmi 读写往返 |
| SOL | App 0x48/0x49/0x4A/0x4B + payload type 1 | pyghmi（含 BREAK、ACK 流控）|
| ASF presence ping | RMCP class 6 | python-ipmi（握手前置）|

细节与踩坑见 [ipmi-lan.md](./ipmi-lan.md) 和 [sol-design.md](./sol-design.md)。

仍缺（只有一项，而且是不该做的）：

- **IPMB 桥接** —— 未实现，也**没有可桥对象**：板上 i2c 没有 0x20 类从机地址，
  NetFn `0x2C` 直接回 `0xC1`（Invalid Command）—— 这正是「没有桥的 BMC」的
  标准应答，不是缺陷。

> 第二十五轮补齐了原来列的另外两项：
> **Set LAN Config 的次要参数**（VLAN / community / auth enable / ARP 控制，
> 全部可读写并持久化）和 **SDR 设备端写入**（Add / Partial Add / Delete /
> Clear SDR，落在 `/config/ipmi-sdr-user.conf` 这个真正的用户仓库里，
> `ipmitool sdr info` 的四项 SDR 能力现在全部是 `yes`）。

第十九轮解决了「开机后 IPMI 不通」，第二十轮补齐用户管理，第二十一轮补齐
Set LAN 写的常用参数与 PEF 配置面，第二十二轮做了一次代码审视（权限提升、
会话回收等 6 处），**第二十四轮补齐 PEF 发送器**：告警目的地表（LAN 参数
17/18/19）、Alert Immediate、SEL 新事件 → syslog + PET（SNMPv1 trap）。
即「无人值守」这条链路已经通了。**第二十五轮**收尾了 LAN 次要参数和 SDR 仓库，
并把守护进程改成静态链接、可从 `/config/firmware/` 原地升级（因为 `/` 是
ramdisk，buildroot 工具链重启就没了）。细节见 [ipmi-lan.md](./ipmi-lan.md)。

> 环境提示：这台机器是 Wi-Fi 接到 BMC 的，短时间连开大量会话时客户端
> 偶尔会因 UDP 丢包重传（现象是 `Session no longer connected`）。
> 设备侧日志显示会话都正常建立，属链路问题，不是守护进程缺陷。

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
| IPMI 用户管理 | 0 | ✅ **第二十轮补齐并实测**（user list/set name/password/priv/enable）|
| 厂商专用（CREANORD/phonehome/bmc-fan） | 22 文件 | ⚪ 本机用不到 |
| 死文件 / 冗余 | 0 | ⚪ 我们的镜像里本来不存在（见 二.C） |

## 七、还不算「缺」但可以做的优化

1. ~~**REST/Redfish 移植**~~ —— 已完成。
2. ~~**IPMI over LAN**~~ —— 已完成（`ubmc_ipmi_lan` + `S47is_ipmi_lan`）。
   继续往深做的话，下一个是 SEL / SDR 传感器桥接。
3. ~~**`S46is_web_fw` 上游化**~~ —— **防火墙部分已完成，路由部分做不到**。
   防火墙例外已经全部上游化：新增 `S44is_fw_conf`（排在 `S45is_mgmtd` 之前）把
   `pub-console`（7681）/`pub-http`（80）/`pub-ipmi`（623）三条规则写进
   `/config/is_mgmtd.conf`，由 mgmtd 自己下发；实测重启后它们落在 INPUT 链
   **中间段**（`58080, 80, 7681, 623, 443, 22, icmp, state, DROP`），不再被
   mgmtd 后下发的 `-i vrf-pub -j DROP` 埋掉。
   剩下两项**做不到**，已确认原因：

   - **nginx 的 `/restapi` `/redfish` 路由**：`/etc/nginx/nginx.conf` 头部写明由
     `is_mgmtd_start_nginx` 生成，模板**编在 `is_mgmtd` 二进制里**；`strings` 只有
     `proxy_pass http://%s:%d;` 和 `.../ws;` 两条，配置树里没有任何可注入 location
     的键，生成的文件里也**没有 `include conf.d`**（只有 `include mime.types;`），
     连"放一个片段进去"的路都没有 → 只能由 `S46is_web_fw` 在生成后补。
     顺带确认 **uhttpd 是用命令行启动的**（`-p 0.0.0.0:58080 -h /opt/silc-web/host/www`，
     没有 `/etc/config/uhttpd`），所以也没法从 uhttpd 侧加 `/restapi` 前缀。
   - **7681 的 hairpin DROP**：这条规则用的是 `-m mac ! --mac-source <vrf-pub MAC>`，
     而 vrf 设备的 MAC 每次开机由内核随机生成（实测 4 次启动 4 个不同值），
     必须运行时读。注意 **mgmtd 自己对 58080 也是这么干的**（同形状的 MAC 规则，
     硬编码在二进制里），所以它没法替我们表达"本机来源"。
     `-m addrtype --src-type LOCAL` 是理论替代，但该内核**没有** addrtype 模块
     （`No chain/target/match by that name`），`-m rpfilter` 同样不可用。
4. ~~**杀掉冗余**~~ —— 出厂遗留的 `.35` 库、`usr/lib64/sa`、`tmpfiles.d`
   在我们的镜像里**本来就不存在**（我们的基线是公开源码自己 buildroot 的产物），
   无需动作，详见「二.C」。
5. **Get/Set User Access** —— **已完成**（第二十轮）。`ipmitool user list /
   set name / set password / priv / enable / disable / test` 全部实测通过，
   账号权限上限会夹住 RAKP 申请的权限，禁用后登录被拒。
   凭据文件升级为 `id:name:password:max-privilege:enabled`，旧的两字段格式兼容。
6. ~~**PEF + LAN alert destination**~~ —— **已完成**（第二十一 + 第二十四轮）。
   0x10/0x11/0x12/0x13/0x14/0x15 实现了配置面，参数 2~5 持久化到
   `/config/ipmi-pef.conf`；LAN 参数 17/18/19 是告警目的地表
   （`/config/ipmi-lan-alert.conf`）；`0x16 Alert Immediate` 发测试 trap；
   SEL 出现新记录（按**时间戳**判断，因为 SEL 是预分配、整表都标 type=0x02）
   时同时推 **syslog**（`LOG_USER`，rsyslog 只保留 `user.*`）和 **PET**
   （SNMPv1 trap，UDP 162，community 取 LAN 参数 16）。
7. ~~**Set LAN Configuration Parameters 的写**~~ —— **已完成全部参数**
   （第二十一 + 第二十五轮）。IP / 掩码 / IP 源 / 默认网关写到
   `/config/is_mgmtd.conf` 并重算 `sha1sum`；auth type、community、ARP 控制、
   gratuitous ARP 间隔写到 `/config/ipmi-lan-params.conf`；VLAN 写
   `interface-list/<if>/vlan`（会真的改管理连通性，日志里有提示）；告警目的地
   写 `/config/ipmi-lan-alert.conf`。由 mgmtd 在下次启动时应用（它才是网络的
   所有者，直接 `ip addr` 会和它打架）。
8. ~~**SDR 设备端写入**~~ —— **已完成**（第二十五轮）。见
   [ipmi-lan.md](./ipmi-lan.md)「用户 SDR 仓库」一节。
