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

- **LAN configuration parameters**（0x0C 0x02）—— 所以 `ipmitool lan print` 能跑通（会自己
  打印 `not supported`），但读不到 IP/掩码/网关的值。要做需要把收包从 `recvfrom` 换成
  `recvmsg` + `IP_PKTINFO` 拿到目的地址，再映射到接口。
- **SHA-256 cipher suite（17/18/19）** —— python-ipmi（`ipmitool.py`）默认挑套件 17，会被
  拒。用 `-o cipher=3` 指定套件 3 即可正常工作。
- 传感器阈值只有 CPU 温度有（`_max`/`_crit`），风扇和 ADC 通道没有阈值可读。
- **主机 → 客户端的 BREAK 通知**：`payload[3] & 0x04`（"收到 BREAK"）不会置位。反向的
  客户端 → 主机 BREAK（`payload[3] & 0x10`）已实现并实测可用。
- 未实现 `Set LAN Config`、`Get/Set User Access`、IPMB 桥接、PEF、SDR 设备端写入。

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

## 和其他客户端的兼容性修正

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
| `ubmc_ipmi_lan.c` | 守护进程（~2200 行，依赖 libcrypto） |
| `S47is_ipmi_lan` | init 脚本（start/stop/restart/status，带守护重启） |
| `probe623.py` | 裸 UDP 探活：发一条合法的 IPMI 1.5 Get Channel Auth Capabilities |
| `ipmi-client-test.py` | pyghmi 验收测试（RMCP+ 握手 + 电源） |
| `ipmi-extras-test.py` | pyghmi 验收：SDR/传感器 / FRU / SEL / bootdev / Reading Factors |
| `ipmi-sol-raw-test.py` | pyghmi 验收：SOL 数据通路（不 ACK 的客户端，验证退化路径） |
| `ipmi-sol-flow-test.py` | pyghmi 验收：SOL 滑动窗口（ACK 的客户端）+ BREAK 转发 |
| `conprobe.c` | 控制台 socket 探针，确认 `/var/run/console_manager_socket` 是双向 fan-out |
| `brktest.c` | BREAK 可行性验证：另开 `/dev/ttyS1` 做 `TIOCSBRK` 是否可行 |
| `apply-runtime.sh` | **不刷机**前提下在真机上部署/调试，仅在开发时用 |
| `deploy-ipmi-lan.ps1` | 开发期把交叉编译产物经 Web RPC 推上真机并重启 |

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

## 踩过的四个坑（都是从「客户端无响应」倒查出来的）

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

```powershell
# 裸探活（不依赖任何 IPMI 库）
python ubmc-build\scripts\ipmi-lan\probe623.py 192.168.31.72

# 完整验收（安装 pyghmi： pip install pyghmi）
python ubmc-build\scripts\ipmi-lan\ipmi-client-test.py 192.168.31.72 root 51L1C0M201B
python ubmc-build\scripts\ipmi-lan\ipmi-client-test.py 192.168.31.72 root 51L1C0M201B --power-on
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

## 调试

```sh
# 真机上
killall ubmc_ipmi_lan
IPMI_LAN_DEBUG=1 IPMI_LAN_BIND= nohup /usr/sbin/ubmc_ipmi_lan >> /var/log/ipmi-lan.log 2>&1 &
tail -f /var/log/ipmi-lan.log
```

`IPMI_LAN_DEBUG=1` 会把每个收到的包和 RMCP+ 头部 hexdump 出来，解密后的 IPMI
消息也会打印（`req (N): ...`）。


