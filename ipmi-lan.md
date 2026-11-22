# IPMI over LAN（RMCP+ / udp 623）

原厂 uBMC 的 IPMI 只有 KCS 通道（`ubmc_ipmi` + `ubmc_ipmi_slave.ko`，给宿主机 KCS 用），
网络上**没有** udp/623 监听者。所以 `ipmitool -I lanplus`、`pyghmi`、任何 Redfish/IPMI
自动化工具都连不上。

本目录下的 `ubmc_ipmi_lan.c` 补上这个监听者，chassis 命令映射到原厂工具：

| IPMI | 落地实现 |
|---|---|
| Get Chassis Status (0x00 0x01) | `ubmc_sys_ctrl.sh -q` |
| Chassis Control (0x00 0x02) | `-f` / `-o` / `-c` / `-r` / `-s` |
| Get/Set System Boot Options (0x00 0x08/0x09) | `/config/ipmi-boot-flags` |
| Activate/Deactivate Payload (0x06 0x48/0x49) | SOL，连 `console_manager_socket` |
| Payload Activation Status / Instance Info (0x06 0x4A/0x4B) | SOL 状态查询 |
| **SOL 字符流**（载荷类型 0x01） | 双向桥接到 `/var/run/console_manager_socket` |
| Get Device ID (0x06 0x01) | 静态 |
| Get Sensor Reading (0x04 0x2D) | 实时读 `/sys/class/hwmon/*` |
| Get SDR Repository Info / Reserve / Get SDR (0x0A 0x20/0x22/0x23) | 启动时按 hwmon 生成 SDR 仓库 |
| Get SEL Info / Reserve / Get Entry / Clear / Get-Set Time (0x0A 0x40…0x49) | 转换 `/var/log/sel` |
| Get FRU Area Info / Read FRU Data (0x0A 0x10/0x11) | 转换 I2C EEPROM 的 ONIE TLV |
| Set Session Privilege (0x06 0x3B) 等 | 静态 |

## 数据源

| 子系统 | 来源 | 说明 |
|---|---|---|
| 温度/风扇/电压 | `/sys/class/hwmon/hwmonN` | 按 `name` 查找：`peci_cputemp.cpu0`（CPU 温度 13 通道）、`silc_fan`（4 风扇 + 3 板温）、`ads7830`（8 路 ADC）。实测 24 个通道有值 |
| SEL | `/var/log/sel` | 持久分区 mmcblk0p7；厂商格式为 **20 字节/条 × 200 槽** 循环缓冲，读到有效记录（`[2]==0x02`）后转成标准 16 字节记录。实测 199 条 |
| FRU | `/sys/bus/i2c/devices/0-0050/eeprom` | 256 字节 ONIE **TlvInfo** EEPROM（不是 IPMI FRU 格式），启动时转成 IPMI FRU 公共头 + Board + Product 三个区域 |
| Boot flags | `/config/ipmi-boot-flags` | 两字节 `flags dev`（十六进制文本） |

### 厂商 SEL 槽格式（20 字节）

```
[0] 00          [1] Record ID   [2] 记录类型（0x02 = system event）
[3] 0xFB        [4..7] 时间戳（大端秒）      [8][9] Generator ID
[10] EvM Rev    [11] 传感器类型  [12] 传感器号  [13] 事件方向/类型
[14..16] Event Data 1/2/3                     [17..19] 填充
```

### 传感器映射

| hwmon | IPMI 传感器类型 | 单位 | M | 说明 |
|---|---|---|---|---|
| `peci_cputemp` temp1..13 | 0x01 Temperature | 0x01 (°C) | 1 | raw = 毫度/1000；`_max`→UNC、`_crit`→UC 作为阈值 |
| `silc_fan` fan1..4 | 0x04 Fan | 0x12 (RPM) | 100 | raw = 毫值/100，M=100 还原成 RPM |
| `silc_fan` temp1..3 | 0x01 Temperature | 0x01 (°C) | 1 | 板温 |
| `ads7830` in0..7 | 0x02 Voltage | 0x04 (V) | 1 | raw = 毫伏/1000 |

## 另外三个「格式」坑

### 6. SDR Full Sensor Record 的 ID 串在偏移 47，不是 49

我最初按"43-46 是正/负滞回，47-48 保留，49 是 ID 串类型/长度"来写，结果
pyghmi 直接 `UnicodeDecodeError`。用两个独立实现交叉验证（pyghmi 的
`sdr.py` 和 `python-ipmi` 的 `SdrFullSensorRecord`）都指向 **47**，
即 LNC(0x2A) 之后紧跟 正滞回(0x2B-0x2C)、负滞回(0x2D-0x2E)、ID 串(0x2F)，
**没有**那两个保留字节。

```
total = 48 + len(name)
record_length = 43 + len(name)
```

### 7. FRU 区域的偏移字段以 8 字节为单位

公共头里 `Board Info Area Starting Offset` / `Product Info Area Starting Offset`
存的是 **8 字节单位数**，不是字节偏移。写 8 会被当成 64。

### 8. FRU 字段的「类型+长度」打包在同一个字节

```
bit7-6 = 类型（0b11 = 8-bit ASCII + Latin1）
bit5-0 = 长度（≤63）
```
后面直接跟数据，**没有**独立的长度字节。字段含义由位置决定
（Board 区第 1 个字段固定是制造商，第 2 个是产品名……），
所以所有文本字段都写 `0xC0 | len`。终止符 `0xC1` 其实就是"类型 3、长度 1"。

## 尚未实现

- **IPMB 桥接** —— 未实现，而且**不该实现**：板上 i2c 没有 `0x20` 类从机地址，
  没有可桥对象；NetFn `0x2C` 直接返回 `0xC1`（Invalid Command），这正是
  「没有桥的 BMC」的标准应答，ipmitool 等工具不会因此报错。

> 用户管理、Set LAN 写的**全部**参数、PEF 配置面、PET/syslog 发送器、
> **SDR 仓库写入** 都已实现，见下面各节。

### Set LAN Configuration Parameters 的写（第二十一 / 二十五轮）

两类落点：**会动网络的**交给 mgmtd 的配置树（它才是网络的所有者，直接
`ip addr` 会和它打架），**纯配置的**进我们自己的小文件。

| 选择子 | 参数 | 落到哪里 |
|---|---|---|
| 0 | Set in Progress | 接受并忽略（客户端会先置 1 再置 0）|
| 1 | Auth Type Support | `/config/ipmi-lan-params.conf` |
| 2 | Auth Type Enable（5 字节）| 同上 |
| 3 | IP Address | `interface-list/<if>/ipv4-address` |
| 4 | IP Address Source | `interface-list/<if>/ipv4-origin`（1→static，2→dhcp）|
| 6 | Subnet Mask | 换算成前缀写 `interface-list/<if>/ipv4-prefix` |
| 10 | BMC ARP Control | `/config/ipmi-lan-params.conf`（默认 `0x02`：应答 ARP）|
| 11 | Gratuitous ARP Interval | 同上（500 ms 为单位，仅存储）|
| 12 | Default Gateway | `system/mgmt/default-gateway` |
| 16 | SNMP Community String | `/config/ipmi-lan-params.conf`，**PET 用它当 community** |
| 17 | Number of Destinations | 只读（按已配的目的地算）|
| 18/19 | Alert Destination 类型/地址 | `/config/ipmi-lan-alert.conf` |
| 20 | 802.1q VLAN ID | `interface-list/<if>/vlan`（0=不打标）|
| 21 | 802.1q VLAN Priority | 接受并忽略（优先级由 socket 决定）|

两个关键点：

- 接口名来自 `IP_PKTINFO` 的**目的地址**（`iface_for_addr()`），所以从哪个口
  写进来就改哪个口的配置。
- 写完必须**重算 `/config/is_mgmtd.conf.sha1sum`**（`conf_rehash()` 调
  `sha1sum`）。mgmtd 启动时 `sha1sum -c` 不过就会删掉 conf 并回滚 `.backup`。

VLAN 这一项是唯一**会真的改变管理连通性**的写：mgmtd 下一次启动会把 eth1
变成带 tag 的子接口，交换机端口没配成同样的 tag 就联系不上 BMC 了（只能上
console）。代码里照实写入，日志会留下提示。

### 用户 SDR 仓库（第二十五轮）

传感器记录是每次启动扫 `/dev/i2c-*` + sysfs 现生成的，**没有可写回的地方**，
所以 `Add SDR` 不能写进"传感器表"。做的是标准做法：一个**独立的用户仓库**。

| 命令 | 行为 |
|---|---|
| `0x20` Get SDR Repository Info | 记录数 = 生成的 + 用户的；空闲字节数真实；最后添加/擦除时间戳 |
| `0x21` Get SDR Repository Allocation Info | info 0/1/2（单元数 / 已用字节 / 空闲字节）|
| `0x22` Reserve | 回 0 |
| `0x23` Get SDR | 先遍历生成的记录（ID 1..N），再遍历用户记录（ID `0x0100` 起）|
| `0x24` Add SDR | 存入用户仓库，**记录 ID 由 BMC 分配**（覆盖请求里的字节）|
| `0x25` Partial Add SDR | 按 `offset`/`in progress` 拼装，最后一片落盘 |
| `0x26` Delete SDR | 只删得掉用户记录；删生成的记录回 `0xCB` |
| `0x27` Clear SDR Repository | 清空用户仓库（`CLR` + `0xAA` 校验）|
| `0x28`/`0x29` | 仓库时间读 / 写（写=接受并忽略）|
| `0x2A`/`0x2B`/`0x2C` | 更新模式进/出、Run Init Agent：空操作（用户仓库在 `/config`，没有"模态"）|

持久化在 `/config/ipmi-sdr-user.conf`（一行一条 hex 记录，第一行是时间戳），
原子写。上限 32 条 × 255 字节。

Operation Support 字节回 `0x0f`（Reserve / Partial Add / Alloc Info / Delete 全支持），
`ipmitool sdr info` 的四行会全部显示 `yes`。之前回的 `0x02` 声称支持
Partial Add 却根本没实现命令，属于谎报。

### 静态链接 + 从 `/config` 更新（第二十五轮）

根文件系统是 **ramdisk**（`root=/dev/ram0`，`/proc/mounts` 里 `/` 是 `rootfs`），
`/usr` 每次开机重建 —— 所以 buildroot 工具链一旦重启就没了，而镜像自带的
glibc 是 **2.21**，任何现代交叉编译器默认要 2.34+。

于是：

- 在 WSL 里用 `arm-linux-gnueabihf-gcc` + `libssl-dev:armhf` 的 `libcrypto.a`
  **静态链接**（`scripts/ipmi-lan/build-cross.sh`），产物 2.4 MB（gzip 后 1.5 MB），
  不依赖镜像里任何 `.so` 版本。
- `S47is_ipmi_lan` 优先跑 `/config/firmware/ubmc_ipmi_lan`，没有才用
  `/usr/sbin/ubmc_ipmi_lan`。`/config` 是真实 ext4 分区，**跨重启保留**，
  所以换版本只要：

```sh
curl -o /tmp/u.gz http://<dev-host>:8011/ubmc_ipmi_lan.gz
gunzip -c /tmp/u.gz > /tmp/u.new && chmod 755 /tmp/u.new
cp /tmp/u.new /config/firmware/ubmc_ipmi_lan.new     # 运行中不能直接覆盖
mv -f /config/firmware/ubmc_ipmi_lan.new /config/firmware/ubmc_ipmi_lan
/etc/init.d/S47is_ipmi_lan restart
```

- `rebuild-image.sh` 也改成优先用 `prebuilt/ubmc_ipmi_lan`，没有才回退到
  buildroot 交叉编译器 —— 这样即使工具链没了也还能出镜像。

### GRATUITOUS ARP / ARP 控制的语义

参数 10 是**位域**（bit1 = 应答 ARP，bit0 = 发 Gratuitous ARP），参数 11 是
**间隔**（500 ms 为单位），不是一个布尔量 —— 早先按布尔处理，`ipmitool lan print`
会显示成 `0.0 seconds` 的"间隔"。现在默认 `0x02`（应答 ARP：内核本来就在做），
Gratuitous ARP 只存储、不主动发（没有实现）。

### 第三个坑：SEL 的「下一条记录 ID」不能往回跳

`Get SEL Info` 报 197 条，`ipmitool sel list` 能列出 192 条，而 pyghmi 的
`get_event_log()` **永远不返回**（一直转，实测 1900 次请求后卡死）。原因不在客户端：

厂商的 SEL 是**定长槽文件**，记录 ID 就是槽号。我们早先在测试里清过 198/199
两个槽，厂商守护进程随后**从低位槽重新开始写**，于是文件顺序变成
`… 0x00c0` 后面跟着 `0x0030 …`。而 `Get SEL Entry` 的「下一条记录 ID」按
**文件顺序**回，客户端按 ID 单向前进分页 → 走到 0xC0 被指回 0x30 → 无限循环。

修法：`sel_load()` 读完后**按记录 ID 排序**，再丢掉 ID 重复的旧记录
（对客户端来说 ID 就是槽的身份，同一个槽里只有最新那条算数）。这样
ID 单调递增、最后一条的 next 是 `0xffff`，分页一定能结束。

实测：`sel info` = 191 条、`sel list` = 191 行、`get_event_log()` = 191 条 / 1.2 秒，
三者一致（修复前分别是 197 / 192 / 不返回）。

> 教训：**客户端卡死不一定怪客户端**。`ipmitool` 不按 ID 分页（它按序号读），
> 所以只有 pyghmi 会暴露这个 bug —— 两个客户端交叉验证才看得出来。

### 第二个 VRF 坑：PET 的发送 socket 也要绑（第二十五轮收尾）

IPMI 的监听 socket 绑到 `vrf-pub` 之后，`pet_send()` 里那个**新开的** socket
仍然是默认表的 —— main 表里连 `192.168.31.0/24` 都没有，于是：

```
ipmi-lan: pet: send to 192.168.31.52 failed: Network is unreachable
```

`Alert Immediate` 照样回 `0x00`，日志里也只是一行，客户端看不出来。
修法：把监听时用的设备名存进 `g_bind_dev`，`pet_send()` 拿到 socket 后
`SO_BINDTODEVICE` 到同一个设备。syslog 那一半不受影响（不涉及路由）。

> 这个坑只有在**重启后**复测才暴露出来 —— 之前手工 restart 守护进程时
> 是好的。所以「刷完机再验一遍」不是形式主义。

### 没有 mkimage 时怎么出镜像

根 fs 是 ramdisk 且 `/root` 也在里面，所以 buildroot 工具链重启即消失，
`rebuild-image.sh` 的 `mkimage` 和原始 `rootfs.cpio.uboot` 一起没了。
第二十五轮的收尾是在**已安装的镜像上打补丁**：

```sh
# 在真机上（cpio/gzip/install 都是 busybox）
sh repack-bank.sh /var/images/r25          # 解包 -> 换文件 -> 重打 -> 校验
#   他做的检查：目标文件在镜像里存在、没有 IS_* 污染、cpio 清单逐条一致、
#   新镜像的 header crc / data crc 自校验通过
# 然后自行 mount /dev/mmcblk0p3、写 rootfs.cpio.uboot、置 is_upgrade 标志
```

`uimage-wrap.py` 复刻了 mkimage 的 64 字节头（`-T ramdisk -C none -a
0x88080000`），支持 `info / verify / wrap`。**先 `verify` 现有镜像**能证明
CRC 算法与 U-Boot 一致（实测 `VERIFY OK`），再 wrap 新镜像才不会做出砖。

保真性对照：把原镜像解开**不改任何文件**再按同样参数重打，得到 54,349,008 B
（原 54,351,610 B，差 2.6 KB，全部来自 gzip 块/inode 顺序），说明解包/重打
是无损的。

生效时机：写入的是**持久配置**，由 mgmtd 在下一次启动时应用到网卡 ——
因为 mgmtd 才是网络的所有者，直接 `ip addr` 改会和它打架。daemon 日志会打印
`lan: eth1 ipv4-address -> x.x.x.x (takes effect when is_mgmtd re-applies ...)`。

### PEF 配置面（第二十一轮）

| 命令 | 说明 |
|---|---|
| 0x10 Get PEF Capabilities | 返回 `51 11 00 00 00`（PEF 1.5，支持 alert/power-down，无过滤器/策略表）|
| 0x11 Arm PEF Postpone Timer | 接受 |
| 0x13 Get PEF Config Param | 1=版本(0x51)、2=动作全局控制、3=动作逐项控制、4=启动延时、5=告警启动延时、6~11=表大小(0)、12=动作支持 |
| 0x12 Set PEF Config Param | 2~5 可写，持久化到 `/config/ipmi-pef.conf` |
| 0x14 / 0x15 | Last Processed Event ID 读/写 |
| **0x16 Alert Immediate** | 往指定目的地发一条**测试 PET**（`petEventType=3`），用来验证 trap 接收端 |

### PEF 发送器 + 告警目的地（第二十三轮）

**告警目的地表**（LAN 参数 17/18/19）已可读写，存在 `/config/ipmi-lan-alert.conf`：

```
# dest:enabled:type:ack_timeout:retries:addr_format:a:b:c:d
1:1:1:0:0:0:192:168:31:52
```

- 参数 17 = 已配置目的地数量（只读）
- 参数 18 = 目的地类型 `[selector, type, ack|retries, addr_format]`，**type 1 = PET**
- 参数 19 = 目的地地址 `[selector, 4 字节 IPv4]`

**事件源与触发**：SEL 文件 `/var/log/sel` 是预分配的（每槽 20 字节、**整表都标成有效记录**），
所以"记录数变多"永远不成立。改为**看时间戳**：空闲 tick 每秒扫一次 SEL，取最大时间戳，
比上次大就对所有更新的记录触发动作（一次最多 16 条，防止刷屏）。

**动作**（`pef_event()`）：
1. `syslog(LOG_WARNING, ...)` —— 落 `/var/log/messages`。
   注意 facility 必须是 **LOG_USER**：这台机器的 rsyslog 会把 `daemon.*` 丢掉，
   只有 `user.*` 进 `/var/log/messages`（实测 `logger -p daemon.warning` 不落盘）。
2. 若 PEF 动作全局控制（参数 2）的 bit0 置位，且配了目的地 → 发 **PET**（SNMPv1 trap，UDP 162）。

**PET 编码**：手写 BER。踩过的坑：

- 外层 SEQUENCE 的长度必须最后回填，一开始把 PDU 的输出偏移和长度混用，
  发出去是 `30 01 ...`（长度 1），接收端直接解析失败
- OID 的多字节子标识里，**续位必须是"除最后一组外都置 0x80"**；
  写反了会把 `1.3.6.1.4.1.3183` 编成 `1.3.6.1.4.1.24.14209`
- `snmpTrapOID.0` 的值必须是 **OID 类型（tag 0x06）**，不能当字符串发
- `petEventData` 是定长 3 字节的 OCTET STRING，不能用 `strlen()` 算长度（`00 00 00` 会变成空串）

实测（`ipmitool`/pyghmi 触发 + 自己写的 [trapd.py](./scripts/ipmi-lan/trapd.py) 解码）：

```
SEQUENCE len 358 / version 0 / community public / PDU 0xa4 (SNMPv1 trap)
enterprise 1.3.6.1.4.1  generic 6  specific 1
  1.3.6.1.2.1.1.3.0             sysUpTime.0        = 0x0001a804
  1.3.6.1.6.3.1.1.4.1.0         snmpTrapOID.0      = 1.3.6.1.4.1.3183.1.1.1
  1.3.6.1.4.1.3183.1.1.1.2.0    petHostname        = ubmc
  1.3.6.1.4.1.3183.1.1.1.3.0    petDateTime        = 261117093853
  1.3.6.1.4.1.3183.1.1.1.5.0    petEventType       = 3 (test) / 2 (event)
  1.3.6.1.4.1.3183.1.1.1.7.0    petEventSourceType = 1 (IPMI SEL)
  1.3.6.1.4.1.3183.1.1.1.8.0    petEventSeverity   = information
  1.3.6.1.4.1.3183.1.1.1.9.0    petSensorType      = 0x37
  1.3.6.1.4.1.3183.1.1.1.10.0   petSensorNumber    = 0
  1.3.6.1.4.1.3183.1.1.1.11.0   petEventData       = 00 00 00
  1.3.6.1.4.1.3183.1.1.1.14.0   petEventDescription = SEL 0: sensor type 0x37 ...
```

syslog 侧同时落盘：

```
2026-11-17T09:44:44 ubmc ubmc-ipmi[20121]:[user.warning] SEL event id 222:
    sensor type 0x07, sensor 13, event 0x6f, data 11 22 33
```

### 用户管理（第二十轮）

支持 `ipmitool user list / set name / set password / priv / enable / disable / test`：

| 命令 | netfn/cmd | 说明 |
|---|---|---|
| Get User Access | App 0x44 | 返回 16 个槽位、当前启用数、priv 与 enable 标志 |
| Set User Access | App 0x43 | 只改权限上限（第三字节是 per-link 权限，本机无 ACL，忽略）|
| Get User Name | App 0x46 | 16 字节，不足补 0 |
| Set User Name | App 0x45 | 可以给**空槽位**起名（真实 BMC 也允许）|
| Set User Password | App 0x47 | `uid, op, password`；**op 0=禁用 1=启用 2=设口令 3=校验** |

凭据文件 `/config/ipmi-lan.passwd` 现在是：

```
# 注释行以 # 开头
id:name:password:max-privilege:enabled
1:root:<password>:4:1
2:is_admin:<password>:4:1
3:opuser:Op3ratorPw:3:1
```

老的 `name:password` 两字段格式仍然能读（自动分配 id、priv=4、enabled）。
口令必须**明文**存放 —— RAKP 用它当 HMAC 密钥，`/etc/shadow` 的哈希用不了。

两个容易踩的点：

- **字节布局**：`Set User Password` 是 `user_id, operation, password` 三个字段，
  不是把 uid 和 op 挤在一个字节里。按挤在一起的写法实现时，ipmitool 的
  `user set password 3 x` 会算成 uid 0 而返回 `0xCC`。
- **operation 语义**：0=禁用、1=启用、2=设口令、3=校验口令。
  按"0=设口令"实现会让 `user set password` 变成禁用用户。
- **Get User Access 的 priv 在第 4 个数据字节**（bits 3:0），enable 标志在
  同一个字节的 bits 7:4。放到第 3 字节时 `ipmitool user list` 会把
  ADMINISTRATOR 显示成 USER。
- 账号没有口令时 `user_password()` 直接返回 NULL，避免用空串当 HMAC 密钥
  让任何人空口令登录。
- RAKP1 里客户端申请的权限会被账号的 `max-privilege` **夹住**（只夹 `s->priv`，
  不夹 `role_byte` —— role byte 参与 RAKP MAC，改了会认证失败）。

顺带补的两条：

- **App 0x54 Get Channel Cipher Suites**：返回 suite 0/1/3/17/18/19 的记录，
  替代原来的 `Invalid command`。ipmitool 在会话建立前会问，缺了它每条命令都会
  先打一行 `Unable to Get Channel Cipher Suites`（1.8.19 的这个查询走的是
  IPMI 1.5 明文路径，本机实测仍会打印该提示，但不再影响任何命令）。
- **App 0x41 Get Channel Access**：只回 2 个数据字节
  （`0x32` = always available + user level auth + per-message auth，`0x04` = admin），
  多了 ipmitool 会报 `Unexpected data length`。App 0x40 Set Channel Access 接受但忽略。

已实现的 LAN 参数（`0x0C 0x02`，`ipmitool lan print` 全部有真实值）：

| 选择子 | 参数 | 来源 |
|---|---|---|
| 0 | Set in Progress | 固定 0 |
| 1 / 2 | Auth Type Support / Enable | 固定 `0x15`（none/一般口令）|
| 3 | IP Address | 请求的**目的地址**（`IP_PKTINFO`）|
| 4 | IP Address Source | `/config/is_mgmtd.conf` 的 `interface-list/<if>/ipv4-origin` |
| 5 | MAC Address | `/sys/class/net/<if>/address` |
| 6 | Subnet Mask | `getifaddrs()` 匹配该地址的接口 |
| 7 | IPv4 Header Params | TTL 64、DF |
| 8 / 9 | Primary / Secondary RMCP Port | 623 / 664 |
| 10 / 11 | ARP Control / Gratuitous ARP | 0 |
| 12 / 13 | Default Gateway IP / MAC | `/proc/net/route` 该接口的默认路由 |
| 14 / 15 | Backup Gateway | 0 |
| 16 | Community String | `public` |
| 17 | Number of Destinations | 0 |
| 20 / 21 | 802.1q VLAN ID / Priority | 0（管理口不打 VLAN）|
| 22 / 23 / 24 | RMCP+ cipher suite 支持 / 列表 / 权限 | suite 3 与 suite 17 |

> 参数选择子是**从 0 开始**的（`ipmitool lan.h` 与 pyipmi 一致：3 = IP 地址、20 = VLAN ID）。
> 早前按 4 = IP 地址去实现会整体错位。

`lan print` 报告的是**请求进来的那个接口**，不是写死 eth1 —— 所以从 192.168.0.x 侧连进来会
看到 eth0 的地址。这是用 `recvmsg()` + `IP_PKTINFO` 拿目的地址实现的。

`IP Address Source` 一度报错成 `static`：原来查的是 busybox `udhcpc` 的
`/var/run/udhcpc.<if>.pid`，但这台机器的 DHCP 是 `is_mgmtd` 自带的客户端在做，那个 pid 文件
永远不存在。实际来源在厂商配置树里：

```
/config/is_mgmtd.conf
  /config/system/mgmt/interface-list/eth0/ipv4-origin,static
  /config/system/mgmt/interface-list/eth1/ipv4-origin,dhcp     ← 管理口
  /config/system/mgmt/interface-list/eth0.4088/ipv4-origin,static
```

现在直接读这棵树（`conf_lookup()`，格式是每行 `<path>,<value>`），`lan print` 正确显示
`dhcp`。没有 mgmtd 配置时才回落到 udhcpc 的 pid 文件。

> `Default Gateway IP` 显示 `0.0.0.0` **不是** bug —— 同一棵树里就是
> `default-gateway,0.0.0.0`，这台机器确实没配默认路由。

## 代码审视（第二十二轮）

把整个守护进程过了一遍，找到并修掉下面这些，**都实测验证过**：

| 问题 | 性质 | 修法 |
|---|---|---|
| `Set Session Privilege Level`（0x3B）不校验账号上限 | **权限提升**：priv 3 的账号直接申请 4 就能拿到管理员 | 按账号 `max-privilege` 夹住，超出返回 `0x81` |
| Chassis Control（0x02）、Clear SEL（0x47）没有权限检查 | **越权**：priv 1 的账号也能切电/清日志 | 要求 priv ≥ 3，否则 `0x80` |
| `lan print` 的 Default Gateway 只读 `/proc/net/route` | 只看得见 main 表，管理口的路由在 VRF 表里 → 写了也读不回来 | 改读 `is_mgmtd.conf` 的 `default-gateway`，与写路径一致 |
| Get Channel Cipher Suites（0x54）索引越界返回 `0xCC` | ipmitool 把它当失败，每条命令都先打一行 `Unable to Get Channel Cipher Suites` | 越界返回**空列表**（只有 channel 字节）；索引按"记录"而非字节计 |
| 用户库写入不是原子的 | 写一半断电会把整个用户库写残 | 临时文件 + `fsync` + `rename` + `sync` |
| 会话槽位没有空闲回收，满了就踢最老的 | 8 个僵尸会话就能把在线监控踢掉 | 加 300 s 空闲过期（SOL 占用的不回收）；`alloc_session()` 给新会话盖时间戳 |

自查时还踩到一个自己引入的坑：空闲回收一开始把 `last == 0`（刚分配、还没收包）当成"最久空闲"，
一秒内就把正在握手的会话删掉 —— 现象是客户端报 `Session no longer connected`。
现在 `last == 0` 明确表示"握手中"，不回收。

### 已知限制（不是 bug，但要知道）

- `MAX_SESSIONS = 8`；8 个都在用时，第 9 个会回收最久未活动的那个。
- 会话空闲 300 s 后由 BMC 关闭（IPMI 允许）。长连接监控需要自己重连。
- 明文口令写在 `/config/ipmi-lan.passwd`（0600）—— RAKP 必须要明文，这是协议决定的。
- `conf_set_line()` 按 512 字节行处理，`is_mgmtd.conf` 的行都远小于此。
- pyghmi 的 `raw_command(netfn=1, ...)` 在客户端侧就失败（`Session no longer connected`），
  同一台机器上 netfn 6/0x0c/4 的 raw 都正常、chassis 的类型化调用也正常 —— 客户端库的怪癖。

## 第二、第三个 cipher suite：17/18/19（SHA-256）

之前只支持 suite 3（HMAC-SHA1）。现在同时支持：

| suite | RAKP 认证 | 完整性 | 加密 |
|---|---|---|---|
| 3 | RAKP-HMAC-SHA1 (1) | HMAC-SHA1-96 (1) | AES-CBC-128 |
| 17 | RAKP-HMAC-SHA256 (3) | HMAC-SHA256-128 (4) | AES-CBC-128 |

响应里回显协商结果，客户端（pyghmi、python-ipmi）都直接按它走。

### 踩到的坑：K1/K2 的常量长度两个客户端不一致

```
K1 = HMAC(SIK, 0x01 重复 N 次)
```

- **ipmitool 和 pyghmi**：N = **20**，写死
- **python-ipmi**：N = **摘要长度**，SHA-256 时是 32

两种都"符合"规范措辞，结果就是同一台 BMC 不可能只服务一个。做法是两个都算出来
（`k1`/`k1_alt`），用客户端**第一个**加密报文去试，哪个能通过校验就锁哪个
（日志 `key derivation variant 0|1 in use`）。客户端永远是先发后收（RAKP4 之后第一条
就是 Set Session Privilege Level），所以一定能锁定。

这条是实测逼出来的：只按 digest 长度实现时 pyipmi 全通、pyghmi 全部 `integrity check
failed`；只按 20 字节实现则反过来。

### 另一个顺带修的：open-session 状态码

"不支持的认证算法" 是 **0x04**，不是 0x03（0x03 是 "Invalid payload type"）。
0x04 在客户端的「算法被拒 → 换下一个套件」集合里，所以就算只支持 suite 3，
客户端也会自动回落而不是直接失败。

未识别的 netfn/cmd 统一返回 `0xC1`（Invalid Command），不会挂住客户端。
收到 `Get SDR` 的 6 字节请求时若长度不足会返回 `0xCC`，pyghmi 会自行改用 size=5 重试。

### SOL 的 BREAK（已实现）

客户端在 SOL 头 `payload[3]` 置 bit4（0x10）即请求 BREAK。服务端另外 `open("/dev/ttyS1",
O_WRONLY)` 后做 `TIOCSBRK`，250 ms 后 `TIOCCBRK` 释放。之所以能这么做：tty 的线路事件不
要求独占读端，所以和 `is_console_manager` 的读并存不冲突。

> ⚠️ **BREAK 会在主机内核上「武装」 SysRq**：实测 BREAK 之后主机打印
> `sysrq: SysRq : HELP : loglevel(0-9) reboot(b) crash(c) ...`——即紧跟着发的那个字符被当成
> SysRq 命令吃掉了。这是 Linux 串口控制台的固有行为（真实 BMC 也一样），但也意味着
> **误发 BREAK 后随手按键可能触发 `b`（重启）或 `c`（crash）**。所以只在客户端明确请求时
> 才发，且长度限制在 250 ms。

### 反方向：主机发来的 BREAK（已实现）

主机发 BREAK 时，`is_console_manager` 读到的只是一个"断帧"，字节流里什么都没有 —— 线路
事件在 tty 层就被消耗掉了。但驱动会为每个端口维护错误计数器，所以：

```
open("/dev/ttyS1", O_RDONLY|O_NOCTTY|O_NONBLOCK)   /* 只开不读，不抢字节 */
ioctl(fd, TIOCGICOUNT, &icount)                     /* icount.brk */
每 500 ms 比对一次，增加就认定主机发了 BREAK
```

检测到之后在下一包 SOL 的 `payload[3]` 置 `0x04`；如果当前没有字符数据可捎带，就单发一个
纯头包把标志带出去。实测收到 `flags=04` 且只带一次。

先确认过这台机器支持：`scripts/ipmi-lan/brkcount.c` 探针输出
`brk=0 frame=0 overrun=0 parity=0 rx=33256 tx=25`，计数器可读且在正常串口流量下不会乱涨。

> 验证方法说明：主机当前没跑系统，无法让它主动发 BREAK。所以上报路径是用一个**临时构建**
> （把基线故意错开 1）验证的，确认客户端收到 `0x04` 后立刻换回正式二进制。

### SOL 的 ACK 流控（已实现）

滑动窗口，最多 4 包（`SOL_WINDOW`）在途，每包 ≤128 字符：

- 客户端 ACK（`payload[1] & 0x0f`）到达 → 丢掉队列里该序号及其前面的包。
- 1 s 没 ACK → 重传队首包，最多 3 次。
- 如果客户端**从未** ACK 过，2 次重传后判定「这个客户端不 ACK」，置 `sol_noack` 退化成
  无窗口直发——否则窗口会让整个控制台永久卡死。

这条退化路径是实测出来的：最初只有纯窗口实现时，一个只发不收的客户端只拿到 652 字节
（4 包 + 首包）就断流，拿不到 login 提示；加上退化后同样客户端能收全 1636 字节的完整横幅。

只实现 cipher suite 3（HMAC-SHA1 认证 + HMAC-SHA1-96 完整性 + AES-CBC-128 加密）——
这是 `ipmitool -I lanplus` 和 pyghmi 的默认组合。

## 与其他客户端的兼容性修正

这些都是拿两个独立客户端（pyghmi、python-ipmi）交叉验证时才暴露出来的：

### ASF Presence Ping（RMCP class 6）

真实 BMC 都会应答。python-ipmi 在连之前先发这个，不应答就直接报
`no response to RMCP ping`。应答内容是 presence pong：IANA `00 00 11 BE`、类型 `0x40`、
回显 tag、16 字节数据（`oem_iana=4542`、`oem_defined=0`、`supported_entities=0x80`（IPMI）、
`interactions=0`）。

### 保密性填充字节必须是 `0x01 0x02 0x03 ...`

IPMI 规定填充字节内容 "don't care"，但 python-ipmi 会按规范示例**校验**它们是递增序列，
填 `0xFF` 会报 `invalid confidentiality pad`。改成递增序列后 pyghmi 和 ipmitool 也照收。

### `Close Session`（App 0x3C）

不实现的话客户端每条命令都成功、最后断开时抛 `CompletionCodeError cc=0xC1`，退出码非 0。
现在返回成功再拆会话。

### FRU 空字段不能省略

FRU 字段是**按位置**解析的。产品区的 Asset Tag 为空时我原来直接跳过，结果后面的
FRU File ID 被当成 Asset Tag，再往后就越界崩在 `_decode_custom_fields`。空字段必须写成
`0xC0`（类型 3、长度 0）。

### `Get Channel Info`（App 0x42）

`ipmitool lan print` 靠它找 802.3 LAN 通道。两个客户端都按
`data[1] & 0x7F` 取介质类型（不是规范里写的高 4 位），响应共 9 个数据字节。
只应答通道 1（以及 `0x0E`/`0x0F` 别名），其余返回 `0xCC` 让客户端跳过。

## 文件

| 文件 | 用途 |
|---|---|
| **生产件** | |
| `ubmc_ipmi_lan.c` | 守护进程（~4400 行，静态链接 libcrypto） |
| `build-cross.sh` | WSL 里静态交叉编译并生成 `prebuilt/`（不依赖 buildroot 工具链） |
| `S47is_ipmi_lan` | init 脚本（start/stop/restart/status，等 eth1 进了 VRF 再绑到 VRF 上启动；优先跑 `/config/firmware/ubmc_ipmi_lan`） |
| `S44is_fw_conf` | 把 7681/80/623 的防火墙例外写进 `/config/is_mgmtd.conf`（必须早于 S45is_mgmtd）|
| `ipmi-selfcheck` | 本机 623 自检（Python 2.6+/3 通用），默认探 eth1 的管理地址，排障用 |
| `repack-bank.sh` | 在真机上把新文件打进**已安装的** bank1 镜像（没有 buildroot 工具链时唯一的出镜像办法）|
| `uimage-wrap.py` | 给 gzip 后的 cpio 加/校验 U-Boot uImage 头（mkimage 的替代，py2.6+ 可用；根 fs 的 python 没有 zlib，用 binascii）|
| **验收测试**（真机跑，带断言）| |
| `probe623.py` | 裸 UDP 探活：发一条合法的 IPMI 1.5 Get Channel Auth Capabilities |
| `ipmi-client-test.py` | pyghmi：RMCP+ 握手 + 电源 |
| `ipmi-extras-test.py` | pyghmi：SDR/传感器 / FRU / SEL / bootdev / Reading Factors |
| `ipmi-sol-raw-test.py` | pyghmi：SOL 数据通路（不 ACK 的客户端，验证退化路径） |
| `ipmi-sol-flow-test.py` | pyghmi：SOL 滑动窗口（ACK 的客户端）+ BREAK 转发 |
| `lanparam_test.py` | LAN 次要参数读写回环（auth / community / ARP / VLAN）|
| `lanparam_persist.py` | LAN 参数跨守护进程重启的持久化验证（write / read / restore）|
| `sdr_lan_test.py` | SDR 用户仓库写入（add / partial add / delete / clear）+ LAN 参数 |
| `pet_community_test.py` | 起本地 trap 接收器，验证 PET 用的是配置的 community |
| `trapd.py` | udp/162 监听 + BER 解码（`pet_community_test.py` 用它）|
| `poll623.py` | 连续探测，量开机到 623 可用的时间 |
| **一次性硬件探针**（保留原因见 sol-design.md / 下文 VRF 一节）| |
| `sockprobe2.c` | VRF 绑定方式对照实验（wildcard / vrf-pub / eth1 / REUSEADDR / PKTINFO）|
| `conprobe.c` | 控制台 socket 探针，确认 `/var/run/console_manager_socket` 是双向 fan-out |
| `brktest.c` | BREAK 可行性验证：另开 `/dev/ttyS1` 做 `TIOCSBRK` 是否可行 |
| `brkcount.c` | `TIOCGICOUNT` 探针：确认 UART 维护 BREAK 计数器（主机→BMC 检测的前提）|

## 凭据为什么单独存

RMCP+ 的 RAKP2/RAKP3 认证码是 **以明文口令为 HMAC 密钥**算出来的：

```
RAKP2 AuthCode = HMAC-SHA1(password, RcID||RmID||Rc||Rm||GUID||role||userlen||user)
RAKP3 AuthCode = HMAC-SHA1(password, Rm||RcID||role||userlen||user)
SIK            = HMAC-SHA1(password, Rc||Rm||role||userlen||user)
```

`/etc/shadow` 里是单向哈希，**算不出**这些 MAC——真实 BMC 从来都是单独存一份 IPMI 口令。
我们放在 `/config/ipmi-lan.passwd`（`/config` 是 mmcblk0p5 的持久 ext4，120 MB），
格式 `user:password`，权限 0600。init 脚本首次启动会播种一个默认口令。

## 踩过的六个坑（都是从「客户端无响应」倒查出来的）

### 1. socket 既不能绑 vrf-pub，也不能开 SO_REUSEADDR

现象：`iptables` 的 `--dport 623 -j ACCEPT` 计数器在涨（包确实进了 INPUT 并被放行），
`tcpdump -i eth1` 也抓到了包，但守护进程 `recvfrom` 一个字节都收不到。

关掉 `SO_REUSEADDR`、只保留一个 wildcard socket 之后立刻正常。对照实验：busybox
`nc -u -l -p 623`（无 REUSEADDR 的 wildcard socket）能收到，说明网络栈和防火墙都没问题。

结论：**保持单个 wildcard socket，不要 `SO_REUSEADDR`，不要 `SO_BINDTODEVICE`。**
（`so_reuseaddr` 下多个绑定同一端口的 socket 会让 VRF slave 上的包匹配不到任何一个。）
`net.ipv4.udp_l3mdev_accept` 保持默认 0 即可，不需要动。

### 2. IPMI 校验和要把校验字节本身算进去

`ipmi_csum(p, n)` 返回 `-(sum(p[0..n-1]))`，所以验证时 n 必须**包含**校验字节：

```c
ipmi_csum(req, 3)            /* rsAddr + netFn + chk          */
ipmi_csum(req + 3, reqlen-3) /* 其余字节 + 结尾 chk            */
```

原来写成 `ipmi_csum(req, 2)`，恒不为 0 → **所有请求都被静默丢弃**。这个 bug 和坑 1
叠加在一起，很容易误判成"包没收到"。

### 3. completion code 是 cmd 之后的第一个字节，不是末尾

IPMI 响应数据 = `[cmd][completion code][data...]`。原来所有 handler 都把
`CC_OK` 追加在数据末尾，客户端只会看到 code = 数据首字节。

### 4. RMCP+ **响应**头里的 Session ID 是客户端的 ID（非对称）

```
请求（控制台→BMC）   Session ID = Managed System Session ID（BMC 在 Open Session Response 里给的）
响应（BMC→控制台）   Session ID = Remote Console Session ID（控制台在 Open Session Request 里给的）
```

pyghmi 在 `session.py:1461` 用 `sid != self.localsid` 校验响应，写 `rm_id` 会被静默丢弃。
RAKP2/RAKP4 的 payload 里也要回 `rc_id`。

### 5. `daemon &` 会挂死 uhttpd（如果调用者拥有管道）

在 Web RPC 的 CGI 里跑 `/etc/init.d/S47is_ipmi_lan start`，如果脚本内部用
`daemon &` 起进程，子进程会**继承 CGI 的 stdout 写端**。uhttpd 要等到管道 EOF
才结束请求，于是永远等下去 —— 表现是「Web UI 整个挂掉」，而且只能重启恢复。

shell 侧的 `supervise > /dev/null 2>&1 < /dev/null &` 在 busybox ash 下**并不可靠**。
正确做法是让守护进程自己 detach：

```c
if (argc > 1 && strcmp(argv[1], "-d") == 0)
    daemonize();      /* fork / setsid / chdir / freopen stderr -> LOG_FILE */
```

init 脚本因此只写 `"$DAEMON" -d`，同步返回；日志由守护进程自己 `freopen` 到
`/var/log/ipmi-lan.log`，完全不依赖调用者的重定向。

### 6. 开机后 IPMI 不通：两条独立的错（都不是内核时序）

现象：从 init 启动时 `ss -ulnp` 能看到 `0.0.0.0:623`，`iptables -vnL INPUT` 的
623 规则计数器在涨，但客户端收不到任何回复；手动 `restart` 一次就好了。
看起来像"开机早期创建的 socket 收不到包"，实际是两个独立错误叠在一起。

#### 6.1 ACCEPT 规则被 mgmtd 的 DROP 埋在下面

原先 `S47is_ipmi_lan` 自己 `iptables -I INPUT <pub-drop 位置>` 插 623 的 ACCEPT。
问题是 mgmtd **稍后**才逐条用 `-I INPUT 1` 下发自己的规则（`pub-state`/`pub-icmp`/
`pub-ssh`/`pub-web`/`pub-web-be`/`pub-drop`），每下发一条就把我们那条往下推一格，
最后 623 落到 `-i vrf-pub -j DROP` 之下 → 静默丢弃。

`restart` 之所以"治好"它：脚本重新插入时 DROP 已经在链里，位置算对了。

**修法**：和 80/7681 完全一样，把 623 写进 `/config/is_mgmtd.conf`（`S44is_fw_conf`），
由 mgmtd 自己按文件顺序装到 DROP 上面。实测重启后
`iptables --line-numbers` 里 623 在第 8 行、`DROP all` 在第 13 行。

#### 6.2 socket 必须绑到 VRF，否则收不到也回不去

eth1（OOB 口）是 `vrf-pub` 的 slave，路由全在 **table 300**，main 表里连
`192.168.31.0/24` 都没有。于是普通的 `0.0.0.0:623` wildcard socket：

- **收**：包在 eth1 上到达，`sdif != 0`。`net.ipv4.udp_l3mdev_accept=0`（mgmtd 的默认）
  时内核不把这种包给 root 表的 wildcard socket —— 防火墙放行、`NoPorts` 增长、
  socket 就在那儿，却一个字节都收不到。
- **发**：即使把 `udp_l3mdev_accept` 置 1 让收方向通了，回复仍按 main 表选路 →
  `ENETUNREACH`，客户端照样超时。

对照实验（同机、同时刻、同一端口）：`sockprobe2 6280:r 6283 623 6284:p` 四个
wildcard socket 全部正常应答，唯独守护进程那条不应答；把 eth1 `nomaster` 掉之后
Web(443)/SSH(22) 还行、**IPMI 立刻 0/20** —— 证明 Web 那几个守护进程本来就活在 VRF 里，
只有 IPMI 的 socket 在默认表。

**修法**：`S47is_ipmi_lan` 先 `wait_ready()` 等到 eth1 **已被 enslave**
（`/sys/class/net/eth1/master` 存在）且有地址，再用
`IPMI_LAN_BIND=<master> ubmc_ipmi_lan -d` 把监听 socket `SO_BINDTODEVICE` 到
`vrf-pub`。这样收方向走 `sk_bound_dev_if == sdif` 命中，发方向自动用 table 300
选源地址和路由，两个方向都不再依赖 `udp_l3mdev_accept`。
（`udp_l3mdev_accept=1` 仍然保留，只服务"拿不到 VRF 名"时的 wildcard 兜底。）

**结果**：刷入第十七轮之后重启，**开机 70 秒** IPMI 就能正常应答，
不需要重启守护进程，也没有任何看门狗。

## 另一个客户端兼容性坑



pyghmi 判断"对端支持 IPMI 2.0"用的是：

```python
if data[1] & 0b10000000 and data[3] & 0b10:   # session.py:1027
```

`data[1]` 是 Get Channel Auth Capabilities 响应的 **Authentication Type Support** 字节，
bit7 在规范里是保留位，但**不置位 pyghmi 就只会走 IPMI 1.5**。所以 `0x38` 的响应里
`data[1] = 0x91`（bit7 + none + straight password），`data[3] = 0x03`（IPMI 2.0 + RMCP+）。
ipmitool 不看这一位，置上无害。

## 验证

仓库里**不写死主机和口令**（这是一个公开仓库）。脚本都从环境变量或命令行参数取：

```powershell
$env:IPMI_HOST = "<bmc>"        # 所有脚本共用
$env:IPMI_USER = "root"          # 默认就是 root，可以不给
$env:IPMI_PASS = "<password>"    # 没有默认值，缺了脚本直接报 usage
$env:TRAP_HOST = "<本机IP>"       # 只有 pet_community_test.py 用

# 裸探活（不依赖任何 IPMI 库，不需要口令）
python ubmc-build\scripts\ipmi-lan\probe623.py

# 完整验收（安装 pyghmi： pip install pyghmi）
python ubmc-build\scripts\ipmi-lan\ipmi-client-test.py
python ubmc-build\scripts\ipmi-lan\ipmi-client-test.py --power-on

# LAN 次要参数读写回环（auth / community / ARP / VLAN）
python ubmc-build\scripts\ipmi-lan\lanparam_test.py

# LAN 参数跨重启持久化：write -> 重启守护进程 -> read -> restore
python ubmc-build\scripts\ipmi-lan\lanparam_persist.py write
python ubmc-build\scripts\ipmi-lan\lanparam_persist.py read
python ubmc-build\scripts\ipmi-lan\lanparam_persist.py restore

# SDR 仓库写入（add / partial add / delete / clear）+ LAN 参数
python ubmc-build\scripts\ipmi-lan\sdr_lan_test.py

# PET 里用的 community 是否就是配置的那条
python ubmc-build\scripts\ipmi-lan\pet_community_test.py
```

主机也可以照旧放在第一个参数上（`sdr_lan_test.py <bmc>`），口令只走环境变量。
守护进程第一次启动时**自己生成**一个随机口令并写进 `/config/ipmi-lan.passwd`
（同时打一行日志），所以镜像里没有任何默认口令。

`ipmitool` 一侧的等价验收（WSL 里跑，注意 `ip route add 192.168.31.0/24 via …`
否则包根本发不出去）：

```sh
B="-H <bmc> -U root -P <password> -I lanplus -C 3"
ipmitool $B mc info; ipmitool $B lan print 1; ipmitool $B sdr info
ipmitool $B sdr list; ipmitool $B fru; ipmitool $B user list 1; ipmitool $B sel list
```

实测输出：

```
=== pyghmi -> 192.168.31.72 (user root) ===
  RMCP+ session established (RAKP1-4 ok)

  get_power()                {'powerstate': 'on'}
  Get Device ID (App 0x01)   {'netfn': 7, 'command': 1, 'code': 0, 'data': bytearray(b' \x81\x02\x10\x02\x0f...')}
  Get Chassis Caps (0x00 0x00) {'netfn': 1, 'command': 0, 'code': 0, 'data': bytearray(b'\x00\x00\x00\x00\x00')}
  Get Chassis Status (0x00 0x01) {'netfn': 1, 'command': 1, 'code': 0, 'data': bytearray(b'\x01@\x00\x00')}
  set_power('on')            {'powerstate': 'on'}
all checks passed
```

第二十五轮的 `ipmitool` 实测片段（节选，全部字段都有值、没有 `Not Available`
的报错行）：

```
$ ipmitool $B sdr info
SDR Version                         : 0x51
Record Count                        : 24
Free Space                          : 8160 bytes
Most recent Addition                : 11/17/26 17:48:24 HKT
Most recent Erase                   : 11/17/26 17:48:24 HKT
SDR overflow                        : no
SDR Repository Update Support       : unspecified
Delete SDR supported                : yes
Partial Add SDR supported           : yes
Reserve SDR repository supported    : yes
SDR Repository Alloc info supported : yes

$ ipmitool $B lan print 1
Auth Type Support       : NONE MD5 PASSWORD
IP Address Source       : DHCP Address
IP Address              : 192.168.31.72
SNMP Community String   : public
BMC ARP Control         : ARP Responses Enabled, Gratuitous ARP Disabled
802.1q VLAN ID          : Disabled
RMCP+ Cipher Suites     : 3,17,6,192,164,163,162,160
```

`ipmitool $B sel list` 197 条全部正常解码（约 10 ms/条）。

## 调试

```sh
# 真机上
killall ubmc_ipmi_lan
IPMI_LAN_DEBUG=1 IPMI_LAN_BIND= nohup /usr/sbin/ubmc_ipmi_lan >> /var/log/ipmi-lan.log 2>&1 &
tail -f /var/log/ipmi-lan.log
```

`IPMI_LAN_DEBUG=1` 会把每个收到的包和 RMCP+ 头部 hexdump 出来，解密后的 IPMI
消息也会打印（`req (N): ...`）。


