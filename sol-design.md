# SOL（IPMI Serial Over LAN）实现方案

[← 返回总览](README.md)

**已实现并实测通过。** 不需要抢串口设备，只要连 `is_console_manager` 已有的分发
socket 即可，与 Web Console **天然共存**。

> 现状（第二十五轮）：`ubmc_ipmi_lan` 已是**静态链接**版本，装在
> `/config/firmware/ubmc_ipmi_lan`（`/usr/sbin` 里那份重启会还原），
> SOL 相关代码路径未变。客户端侧用 `ipmitool -I lanplus sol activate` 或
> pyghmi 的 `Console` 都能用。

实测（`scripts/ipmi-lan/ipmi-sol-raw-test.py`，一个只发不收、从不 ACK 的客户端）：

```
Get Payload Activation Status  '0000000000'
Get Payload Instance Info      '000000000000000000000000'
Activate Payload (SOL)         code 0  maxout=128 port=623
sending CR through SOL ...
got 17 SOL packet(s), 1636 host byte(s)
    | *********...****************
    |             USENEODDXTX0101USZZ01 login:
Deactivate Payload             'ok'
all SOL checks passed
```

同时 `/ws` 仍返回 101，Web Console 不受影响。

`scripts/ipmi-lan/ipmi-sol-flow-test.py` 再验一遍**会 ACK** 的客户端（走滑动窗口，
无重传、无重复序号）以及 **BREAK 转发**（日志出现 `sol: BREAK asserted for 250 ms`，
之后控制台仍可用）。

## 实现时踩到的四个坑

### A. 外层 payload 分发漏了 `PT_SOL`

`handle_rmcp_plus()` 里是 `ptype = pkt[5] & 0x3f` 再 switch，SOL 的 0xC1 掩码后是
0x01，但 switch 只写了 `case PT_IPMI`（0x00）→ SOL 包全落到 `default` 被丢掉，
日志里只看到 `unhandled payload type 0x01`。

### B. `freopen` 之后 stderr 变成全缓冲

`daemonize()` 里 `freopen(LOG_FILE, "a", stderr)` 会给流套上**块缓冲**，于是启动日志
4 KB 才刷一次 —— 表现为"日志文件是空的"，我还因此误读了几轮**旧进程留下的**日志。
修法是 `setvbuf(stderr, NULL, _IONBF, 0)`。

### C. 512 字节输出缓冲会丢主机数据

主机一次输出 1.5 KB 启动横幅，缓冲只有 512 → `output buffer full, dropping host data`。
改成 4 KB 且**读满就停止读取**（流式 socket 天然背压，内核队列不会丢），
不再主动丢字节。

### D. pyghmi 的 `Console` 在脚本里跑不起来

`console.Console(...)` 的 `Session` 是事件驱动的，且依赖交互式 stdin；脚本里
`sessioncontext` 一直是 `None`。测试改用 `Command` + 直接
`ipmi_session.send_payload(payload_type=1)`，并在等待期间调用
`Session.wait_for_rsp()` 泵 socket —— 否则收不到任何 SOL 包。

## 一、现有控制台链路（实测确认）

```
主机 x86 串口
   ↓ /dev/ttyS1
is_console_manager  (pid 1485: -H /dev/ttyS1 -O /dev/ttyS0)
   ├→ /var/log/host_serial.log          (Console Log 页)
   ├→ /dev/uart-sim → uart_sim_ubmc.ko → /dev/ttyF0   (只写，权限 0200)
   └→ /var/run/console_manager_socket   ★ 双向、广播式 fan-out
          ↑  (3 个 client 同时连着，每个 VRF 一个)
   serial_socket_client（ttyd 的子进程）
          ↓
      ttyd :7681 → nginx /ws → 浏览器
```

关键点：**不是** ttyd 直接开串口，而是 `is_console_manager` 做多路分发。
`serial_socket_client` 里能看到两个字符串：`/var/run/console_manager_socket`
和 `/dev/ttyF0`。

### 实测证据（`conprobe`，见 `scripts/ipmi-lan/conprobe.c`）

```
$ /tmp/conprobe -t 4                 # 只读
connected to /var/run/console_manager_socket
  (timeout, nothing more)            # 主机停在 login，没输出，符合预期

$ /tmp/conprobe -w $'\r' -t 4        # 写一个回车
connected to /var/run/console_manager_socket
wrote 1 of 1 bytes: 0d
  3 bytes: 0d 0d 0a
  48 bytes: ****...                 # 主机回吐出整段启动横幅
  ...
  29 bytes: USENEODDXTX0101USZZ01 login: |
  (timeout, nothing more)
  日志行数: 59824 → 59843            # ★ 同时 /var/log/host_serial.log 正常增长
```

结论：
- socket 是 **双向** 的（写进去的字节到了主机，主机的输出流回来了）
- socket 是 **广播** 的（另开一个 client 不影响 ttyd，日志照常记录）
- **不需要** `SO_REUSEADDR`/设备独占问题；也不会把 WebConsole 的字节"抢走"

## 二、需要实现的协议部分

### 2.1 NetFn App (0x06) 的四个命令

| cmd | 名称 | 请求 | 响应 |
|---|---|---|---|
| 0x48 | Activate Payload | `type(1) instance(1) flags(1) 0 0 0` | `0 0 0 | maxout(2 LE) | maxin(2 LE) | port(2 LE) | vlan(2 LE)` |
| 0x49 | Deactivate Payload | `type(1) instance(1) 0 0 0 0` | 空 |
| 0x4A | Get Payload Activation Status | `type(1)` | `instance1_state instance2_state 0 0 ...` |
| 0x4B | Get Payload Instance Info | `type(1) instance(1)` | `session_id(4 LE) 0 0 0 0 0 0 0` |

pyghmi 的调用序列（`console.py`）：

```
netfn=6 cmd=0x48 data=(1, 1, 0xC0, 0, 0, 0)      # SOL, instance 1, encrypt+auth, CTS ok
  → 失败码语义：0x80 = 已有别的 client 占用
              0x81 SOL disabled / 0x82 达到最大会话数
              0x83 不能加密 / 0x84 必须加密
netfn=6 cmd=0x4B data=(1, 1)                     # 之后作为 keepalive 反复发
netfn=6 cmd=0x49 data=(1, 1, 0, 0, 0, 0)         # 退出时
```

`0x48` 响应里的 **端口必须等于 623**，否则 pyghmi 直接抛
`NotImplementedError("Non-standard SOL Port Number")`（它同时接受大小端两种写法）。

### 2.2 RMCP+ 载荷类型

SOL 不是新的 UDP 端口，而是**同一个会话、同一个 623 端口**上换一个载荷类型字节：

```
payload type = 0x01 (SOL)  →  实际线路上是 0x41 (完整性) 或 0xC1 (完整性+加密)
IPMI 消息是 0x00          →  0x40 / 0xC0
```

加密和完整性**完全复用现有 session 的 k1/k2**，所以 `handle_encapsulated()`
里除了 `PT_IPMI` 之外再加一个 `PT_SOL` 分支即可，AES/HMAC/padding 一行都不用改。

### 2.3 SOL 载荷格式（两个方向都是 4 字节头 + 数据）

**BMC → 控制台**（pyghmi 的 `_got_sol_payload` 逐字节这么解）：

```
[0] bits3:0  本包序列号（0 = 纯 ACK，没有数据）
[1] bits3:0  确认的控制台序列号
[2]          Accepted Character Count（本包带了多少字节）
[3] bit6 NACK / bit5 主机断电 / bit4 payload 已停用 / bit2 检测到 BREAK
[4..]        字符数据（原样字节，不加密这层就结束了）
```

**控制台 → BMC**（pyghmi 的 `_sendoutput` 这么发）：

```
[0] 序列号 1..15（0 不用）
[1] 确认的 BMC 序列号
[2] Accepted Character Count
[3] 0x10 = BREAK，其余 0
[4..] 字符数据
```

纯 ACK 包就是 `(0, 对端seq, 收到字节数, 0)`。

## 三、代码改动点

全部在 `scripts/ipmi-lan/ubmc_ipmi_lan.c`，预计 +300 行左右：

### 3.1 会话结构体

```c
typedef struct {
    ...
    /* SOL */
    int      sol_active;      /* 0/1 */
    int      sol_console_fd;  /* 连到 console_manager_socket 的 fd */
    uint32_t sol_owner_rcid;  /* 占用 payload 的会话 */
    uint8_t  sol_tx_seq;      /* 1..15 */
    uint8_t  sol_last_rx_seq; /* 客户端序列号，用于去重/ACK */
    uint8_t  sol_pending[MAX_PKT];   /* 已发但未 ACK 的数据 */
    int      sol_pending_len;
    uint8_t  sol_outbuf[MAX_PKT];    /* 待发给客户端的主机输出 */
    int      sol_outlen;
} session_t;
```

### 3.2 新增函数

| 函数 | 作用 |
|---|---|
| `sol_activate(s, flags)` | 连接 `/var/run/console_manager_socket`（`AF_UNIX SOCK_STREAM`，非阻塞），失败返回 0x81 |
| `sol_deactivate(s)` | 关 fd、清状态 |
| `sol_pump(fd, s)` | 从 console fd 读，塞进 `sol_outbuf`，攒够就 `sol_send_payload()` |
| `sol_send_payload(s, data, len, seq)` | 组 4 字节头 + 数据，走现有 `rmcp_plus_hdr()` + AES + HMAC |
| `sol_recv_payload(s, payload, len)` | 解析客户端包：把 `[4..]` 写进 console fd；`[3]&0x10` 时尝试发 BREAK；回 ACK |

### 3.3 主循环改动

```c
/* select 里加上 console fd，并把超时从 1s 降到 20ms（SOL 有数据时要及时 flush）*/
FD_SET(s->sol_console_fd, &rfds);
tv.tv_usec = sol_active ? 20000 : 0;
...
if (sol_active && FD_ISSET(s->sol_console_fd, &rfds))
    sol_pump(fd, s);
```

`character accumulate interval` / `character send threshold` 按 IPMI 默认值
（100 ms / 2 字节）实现：攒够 2 字节或超过 20 ms 就发一包。

### 3.4 dispatch 里加 4 个 case

```c
case 0x48: /* Activate Payload */   ...
case 0x49: /* Deactivate Payload */ ...
case 0x4A: /* Get Payload Activation Status */ ...
case 0x4B: /* Get Payload Instance Info */ ...
```

`handle_rmcp_plus()` 里 `ptype` 掩码现在是 `pkt[5] & 0x3f`，0x41 会得到 0x01、
0xC1 得到 0x01，所以只需要在 switch 里加：

```c
case PT_SOL: handle_sol(fd, to, pkt, len); break;   /* 0x01 */
```

## 四、风险与对策

| 风险 | 对策 |
|---|---|
| **破坏 Web Console** | 只 `connect()` 读，不写任何设备文件；ttyd 用的是同一个 socket 的另一个连接，fan-out 广播已验证互不影响 |
| **把按键敲到主机上** | SOL 只在 `sol_active` 时才写 socket；未激活时绝不写 |
| **BREAK 无法通过 unix socket 表达** | 先忽略（不报错），文档标注；`ipmitool sol activate` 的 BREAK 主要用于进 BIOS setup，非必需 |
| **多客户端抢 SOL** | `sol_active` 单例，第二个请求返回 0x80 |
| **客户端不 ACK 导致重发风暴** | 保留 `sol_pending`，只在收到新序列号或超时（500 ms）才重发一次，最多 3 次 |
| **select 超时从 1 s 降到 20 ms 增加 CPU** | 仅在 SOL 激活时降；否则维持 1 s |
| **改坏 daemon 失去 IPMI** | 沿用已验证的流程：编译 → base64 推送 → `mv` 原子替换 → pyghmi 回归测试 → 失败则回滚旧二进制（设备上保留 `ubmc_ipmi_lan.bak`） |

## 五、验收方式

```python
from pyghmi.ipmi import console
import time
con = console.Console(bmc="<bmc>", userid="root", password="<ipmi-password>")
time.sleep(2)                       # 应该看到主机 login 提示符
con.send_keys("\r")                 # 回车 → 主机回吐横幅
time.sleep(2)
con.send_keys("root\r")             # 输用户名
time.sleep(2)
con.close()
```

判定：能在 stdout 里看到 `USENEODDXTX0101USZZ01 login:` 和后续交互，
且同一时间浏览器 Web Console 仍然可用（并行验证 fan-out）。

## 六、实现步骤（当时的工作量估计 —— **已全部完成**）

| 步骤 | 内容 | 状态 |
|---|---|---|
| 1 | 会话结构 + `sol_activate`/`sol_deactivate` + 4 个 NetFn case | ✅ |
| 2 | `sol_send_payload`/`sol_recv_payload`（复用现有 AES/HMAC 封装） | ✅ |
| 3 | 主循环接 console fd + 累积/超时逻辑 | ✅ |
| 4 | pyghmi `Console` 实测，按报错迭代序列号/ACK 细节 | ✅（+ [8.2 节](#82-ack-滑动窗口)滑窗）|
| 5 | 写进 `rebuild-image.sh` 6.7 节、更新文档、出 v9 镜像 | ✅（现在出到 v25）|

第 4 步当时被判断为"主要不确定点"，实际踩到的坑确实都在这：
序号回绕、以及"256 个未确认包之后必须等 ACK"（见 8.2）。

## 七、为什么不走"直接开 /dev/ttyS1"

- `/dev/ttyS1` 已被 `is_console_manager` 独占读取；串口是**单读者**语义，
  第二个进程读会**抢走字节**，直接破坏 Web Console
- `/dev/ttyF0` 是 `crw--w----`（只写），本来就是给"注入"用的，读不出主机输出
- 所以只有 `console_manager_socket` 这条路既能读又能写，且天然多客户端

## 八、BREAK 与 ACK 流控（后续补齐，均已实测）

### 8.1 BREAK 怎么发出来的

`console_manager_socket` 是流式 socket，**表达不了串口 BREAK**。但 BREAK 是 tty 的
**线路事件**，不需要独占读端——所以额外 `open("/dev/ttyS1", O_WRONLY)`，只做
`ioctl(TIOCSBRK)` / `ioctl(TIOCCBRK)` 是可行的，和 `is_console_manager` 的读并存不冲突
（可行性先用 `scripts/ipmi-lan/brktest.c` 单独验证过）。

```
客户端置 payload[3] & 0x10
  → sol_break_assert():  open /dev/ttyS1 O_WRONLY, TIOCSBRK, 记下释放时刻
  → 主循环每轮 sol_break_service(): 到点后 TIOCCBRK 并关闭 fd
```

取 250 ms：足够长，主机侧能识别成 framing error；又足够短，不会把 SysRq 一直压住。

> ⚠️ **BREAK 会在主机内核上「武装」 SysRq。** 实测 250 ms BREAK 之后，主机立刻打印
> `sysrq: SysRq : HELP : loglevel(0-9) reboot(b) crash(c) ...`，也就是**紧跟着的那个字符
> 被当成 SysRq 命令吃掉了**（我用 `\r` 试的，结果回车变成了"打印帮助"）。这是 Linux 串口
> 控制台的固有行为，真实 BMC 一样，但也意味着**误发 BREAK 后随手按键可能触发 `b` 重启
> 或 `c` crash**。所以只在客户端明确请求时才发。

反方向（主机发出 BREAK → 通知客户端，`payload[3] & 0x04`）也已经实现：主机发 BREAK 时
console socket 的字节流里什么都没有（tty 层把线路事件吞掉了），但驱动给每个端口维护的
错误计数器还在，所以只打开 `/dev/ttyS1` 而不读它、每 500 ms 用 `TIOCGICOUNT` 比对
`icount.brk`，涨了就在下一包 SOL 的 `payload[3]` 上置 `0x04`；没有字符数据可捎带时单发一个
纯头包把标志带出去。实测客户端收到 `flags=04` 且只带一次。

（主机当前没跑系统，无法让它主动发 BREAK，所以上报路径是用一个"基线故意错开 1"的临时
构建验证的，确认后立刻换回正式二进制。检测本身由 `scripts/ipmi-lan/brkcount.c` 探针确认
可用：`brk=0 frame=0 overrun=0 rx=33256`。）

### 8.2 ACK 滑动窗口

```
最多 SOL_WINDOW=4 包在途，每包 ≤ SOL_MAX_OUT=128 字符
收到 ACK（payload[1] & 0x0f = 客户端最后收到的序号）→ 丢掉该序号及之前的包
1 s 没 ACK（SOL_RETRY_MS）→ 重传队首，最多 SOL_MAX_TRIES=3 次
```

**关键：客户端不 ACK 时必须退化，否则控制台会永久卡死。**

最初只实现了纯窗口，结果一个只发不收的客户端 6 秒内只拿到 652 字节（首包 + 4 包窗口）
就断流，连 login 提示都看不到——因为窗口满了之后服务端一直在傻等 ACK。现在的做法是：
如果这个客户端**从头到尾一个 ACK 都没发过**，重传 2 次后就置 `sol_noack = 1`，
退化成无窗口直发（不再入队、不再重传）。同一条测试脚本立刻从 652 字节变成完整的
1636 字节横幅 + `USENEODDXTX0101USZZ01 login:`。

有 ACK 历史的客户端仍按 3 次重传后丢弃该包继续。

### 8.3 一个测试自身的坑

`ipmi-sol-flow-test.py` 第一版在 BREAK 之后复用 `seq=1` 发回车，服务端按重复包直接丢弃
（`seq == sol_rx_seq` 就不写 console），表现为"BREAK 之后控制台死了"。真实客户端是递增
序号的，改成 1..15 循环后即正常。

