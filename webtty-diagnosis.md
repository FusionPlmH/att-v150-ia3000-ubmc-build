# 网页 Console（WebTTY）诊断

## 结论

**Console Shell 打不开的根因不在权限，而在 nginx 连不上 ttyd。**

固件自己的 nginx 错误日志给出了直接证据：

```
[error] connect() failed (110: Connection timed out) while connecting to upstream,
        request: "GET /ws HTTP/1.1", upstream: "http://0.0.0.0:7681/ws",
        host: "192.168.31.72"
        2026/11/21 08:51:37  client: 192.168.31.74
        2026/11/21 08:53:50  client: 192.168.31.74
```

`192.168.31.74` 是操作者本机，两个时间点正是他打开 Console 页的时刻。
错误是 **110 ETIMEDOUT**（不是 refused），说明 SYN 发出了但没有回程。

## 链路与各环节状态

```
/admin/bmc/shell  →  action_console()
     ├─ check_normal()  要求 user_privil >= PRIVILEGE_ADMIN (3)
     ├─ 写 /tmp/ubmc-web-console-token/<token>
     └─ 渲染 console.htm（xterm.js）
              ↓
         wss://host/ws   （子协议 "tty"）
              ↓  nginx: location ^~ /ws → proxy_pass http://0.0.0.0:7681/ws
              ↓  首帧 {"AuthToken":"<token>"}
         ttyd 1.5.2-bd9de8c（libwebsockets 3.0.1，Silicom 定制 fork）
              ↓
         /usr/bin/serial_socket_client → /dev/ttyS1（主机串口）
```

| 环节 | 状态 | 证据 |
|---|---|---|
| Console 页权限 | ✅ 正常 | `is_admin` 登录时生成了 token（08:49、08:55 两个文件）|
| root 访问 Console 页 | ⚠️ 被拒 | `check_normal()` 要 ≥3，root 是 0 → `admin/forbidden`（2,849 B）|
| ttyd 二进制 | ✅ 能起 | `NOTICE: Creating Vhost 'default' port 7681` |
| **nginx → ttyd** | ❌ **连不上** | 上述 ETIMEDOUT |
| Console **Log**（另一个功能）| ✅ 正常 | `/var/log/host_serial.log` 2.7 MB |

> 注意区分两个功能：
> - `admin/bmc/shell` → Console **Shell**（ttyd 交互终端）← 坏的就是这个
> - `admin/bmc/console` → Console **Log**（分页查看 `host_serial.log`）← 正常

## 根因推断：VRF

这台机器的网络是 **VRF 架构**：

```
ip vrf show
  vrf-ib        100
  vrf-ib-vlan   200
  vrf-pub       300

ss -tlnp
  nginx   0.0.0.0%vrf-pub:443      nginx   0.0.0.0%vrf-ib:443      ...
  sshd    0.0.0.0%vrf-pub:22       uhttpd  0.0.0.0%vrf-pub:58080   ...
```

**`nginx` / `sshd` / `uhttpd` 都绑定到具体 VRF**，而系统启动时的 ttyd
在 `/proc/net/tcp` 里是裸 `0.00000000:1E01`，`ss -tlnp` **根本列不出来**
（因为 ss 只显示当前 VRF 的套接字）—— 即 ttyd 落在默认网络表里，
不在任何 VRF。

于是 nginx（vrf-pub）向 `0.0.0.0:7681` 发起连接时，在 table 300 里
找不到路由，SYN 无回程 → ETIMEDOUT。

同一条链上的 `uhttpd:58080` 能通，正是因为它绑在 `0.0.0.0%vrf-pub`。

## 修复方向

`is_mgmtd` 里起 ttyd 的代码是硬编码字符串：

```
strings is_mgmtd | grep ttyd
  killall -q uhttpd ttyd
  ttyd %s &
```

`%s` 只是命令（`/usr/bin/serial_socket_client`），没有端口/VRF 参数。
三条可行路线：

1. **让 ttyd 进 VRF**（最贴合现有架构）
   把启动命令改成 `ip vrf exec vrf-pub ttyd %s &`
   —— 需要改 `is_mgmtd` 源码或二进制里的这个字符串

2. **让 nginx 指向 VRF 内地址**
   `proxy_pass http://<vrf-pub 地址>:7681/ws;`
   —— 但 `nginx.conf` 由 mgmtd 每次启动时生成，要改生成模板

3. **ttyd 显式绑 VRF 地址**
   `ttyd -i <vrf-pub 地址> ...` —— 同样要改启动命令

三条都要动 mgmtd（闭源二进制），或者在外层包一层。

## 一个方法论上的坑（重要）

**在设备内用 `/rpc/sys` 起的 shell 里做 curl 测试是无效的。**

实测：从该 shell 里 curl

```
http://192.168.31.72:58080/   → 000     ← uhttpd，外面访问是正常的
https://192.168.31.72/        → 000     ← nginx，外面访问是正常的
http://192.168.31.72:7681/    → 000
```

三个全 000，包括两个明知能用的服务。说明该 shell 的出向连接被隔离
（VRF / conntrack），**不能用来判断任何本地服务是否可用**。

所以：

- 判断服务可用性，只能从**外部**测（Windows 侧 curl）
- 判断 nginx 到上游的连通性，只能看 **`/tmp/nginx_error.log`**
- 不要用 `curl 127.0.0.1` / `curl 0.0.0.0` 在设备内自测 —— VRF 下 127.0.0.1
  在 VRF 表里没有路由，`ping 127.0.0.1` 都是 100% 丢包

## 与自编译版的关系

ttyd 二进制本身我没动过（仍是原厂 `ttyd 1.5.2-bd9de8c`），
`nginx.conf` 由同一个 mgmtd 生成，`is_mgmtd` 只有
`.admin_shadow` 一个字段被改过。

**所以这个 bug 在原厂固件和自编译版上应该是一样的**
—— 它不是移植引入的回归，是固件自身的问题。

要确认可从 bank 0 启动原厂 1.2.0.8 后打开 Console 页，看
`/tmp/nginx_error.log` 是否出现同样的 110 错误。
