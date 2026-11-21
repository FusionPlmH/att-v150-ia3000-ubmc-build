# IPMI over LAN（RMCP+ / udp 623）

原厂 uBMC 的 IPMI 只有 KCS 通道（`ubmc_ipmi` + `ubmc_ipmi_slave.ko`，给宿主机 KCS 用），
网络上**没有** udp/623 监听者。所以 `ipmitool -I lanplus`、`pyghmi`、任何 Redfish/IPMI
自动化工具都连不上。

本目录下的 `ubmc_ipmi_lan.c` 补上这个监听者，chassis 命令映射到原厂工具：

| IPMI | 落地实现 |
|---|---|
| Get Chassis Status (0x00 0x01) | `ubmc_sys_ctrl.sh -q` |
| Chassis Control (0x00 0x02) | `-f` / `-o` / `-c` / `-r` / `-s` |
| Get Device ID (0x06 0x01) | 静态 |
| Get Chassis Caps (0x00 0x00)、Set Session Privilege (0x06 0x3B) 等 | 静态 |

只实现 cipher suite 3（HMAC-SHA1 认证 + HMAC-SHA1-96 完整性 + AES-CBC-128 加密）——
这是 `ipmitool -I lanplus` 和 pyghmi 的默认组合。

## 文件

| 文件 | 用途 |
|---|---|
| `ubmc_ipmi_lan.c` | 守护进程（~1000 行，依赖 libcrypto） |
| `S47is_ipmi_lan` | init 脚本（start/stop/restart/status，带守护重启） |
| `probe623.py` | 裸 UDP 探活：发一条合法的 IPMI 1.5 Get Channel Auth Capabilities |
| `ipmi-client-test.py` | pyghmi 验收测试（完整 RMCP+ 握手 + 真实命令） |
| `apply-runtime.sh` | **不刷机**前提下在真机上部署/调试，仅在开发时用 |

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

## 尚未实现

Redfish 侧已经能用（见 `web-stack.md`），IPMI 侧目前只覆盖电源与设备信息。缺：

- **SEL**（`ipmitool sel list`）—— 需要把 `/var/log/host_serial.log` 或 syslog 桥接成 SEL 记录
- **SDR / 传感器读数**（`ipmitool sensor`）—— 需要传感器 SDR 仓库
- **FRU**（`ipmitool fru`）—— 机箱序列号等信息原厂在 CLI `show device` 里有，可静态编入
- **SOL**（`ipmitool sol activate`）—— 串口控制台目前走 ttyd/nginx `/ws`
- **Boot device 设置**（`ipmitool chassis bootdev`）

未识别的 netfn/cmd 统一返回 `0xC1`（Invalid Command），不会挂住客户端。
