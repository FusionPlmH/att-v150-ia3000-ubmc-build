# SOL（IPMI Serial Over LAN）实现方案

**已实现并实测通过。** 不需要抢串口设备，只要连 `is_console_manager` 已有的分发
socket 即可，与 Web Console **天然共存**。

实测（`scripts/ipmi-lan/ipmi-sol-raw-test.py`）：

```
Get Payload Activation Status  '0000000000'
Get Payload Instance Info      '000000000000000000000000'
Activate Payload (SOL)         code 0  maxout=128 port=623
sending CR through SOL ...
got 3 SOL packet(s), 94 host byte(s)
    | USENEODDXTX0101USZZ01 login:
Deactivate Payload             'ok'
all SOL checks passed
```

同时 `/ws` 仍返回 101，Web Console 不受影响。

## 实现时踩到的三个坑

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
con = console.Console(bmc="192.168.31.72", userid="root", password="51L1C0M201B")
time.sleep(2)                       # 应该看到主机 login 提示符
con.send_keys("\r")                 # 回车 → 主机回吐横幅
time.sleep(2)
con.send_keys("root\r")             # 输用户名
time.sleep(2)
con.close()
```

判定：能在 stdout 里看到 `USENEODDXTX0101USZZ01 login:` 和后续交互，
且同一时间浏览器 Web Console 仍然可用（并行验证 fan-out）。

## 六、工作量估计

| 步骤 | 内容 |
|---|---|
| 1 | 会话结构 + `sol_activate`/`sol_deactivate` + 4 个 NetFn case |
| 2 | `sol_send_payload`/`sol_recv_payload`（复用现有 AES/HMAC 封装） |
| 3 | 主循环接 console fd + 累积/超时逻辑 |
| 4 | pyghmi `Console` 实测，按报错迭代序列号/ACK 细节 |
| 5 | 写进 `rebuild-image.sh` 6.7 节、更新文档、出 v9 镜像 |

第 4 步是主要不确定点（序列号/ACK 的边界情况），但协议格式已经从 pyghmi 源码
逐字节确认，风险可控。

## 七、为什么不走"直接开 /dev/ttyS1"

- `/dev/ttyS1` 已被 `is_console_manager` 独占读取；串口是**单读者**语义，
  第二个进程读会**抢走字节**，直接破坏 Web Console
- `/dev/ttyF0` 是 `crw--w----`（只写），本来就是给"注入"用的，读不出主机输出
- 所以只有 `console_manager_socket` 这条路既能读又能写，且天然多客户端
