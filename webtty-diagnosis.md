# 网页 Console（WebTTY）诊断与修复

## 结论（已实测验证）

**根因是 iptables，不是 VRF。** `is_mgmtd` 给 ttyd 起了服务，却没给它的
端口 7681 补防火墙例外，nginx 到上游的连接被 ATT 默认配置里的兜底
`DROP` 规则吃掉。同一类疏漏还导致 nginx 的 80 端口（只做 301 跳转）
也连不上。

这是**原厂设计缺陷**，出厂固件 1.2.0.8 与自编译版都存在，与我们的
刷机/接入方式无关。

修复：新增 `etc/init.d/S46is_web_fw`（已在设备上 A/B 验证通过）。

## 症状

Web UI → System > BMC > **Console (Shell)** 页面一直转圈，终端不出来。
同页签下的 **Console (Log)** 正常（`admin/bmc/console` 只是分页显示
`/var/log/host_serial.log`，和 ttyd 无关）。

固件自己的 nginx 错误日志给了第一条线索：

```
[error] connect() failed (110: Connection timed out) while connecting to upstream,
        request: "GET /ws HTTP/1.1", upstream: "http://0.0.0.0:7681/ws",
        host: "192.168.31.72"
        2026/11/21 08:51:37  client: 192.168.31.74
        2026/11/21 08:53:50  client: 192.168.31.74
```

`192.168.31.74` 是操作者本机，两个时间点正是打开 Console 页的时刻。
注意是 **110 ETIMEDOUT** 而不是 `ECONNREFUSED`（111）—— 这个区别后来
成了破案的关键。

## 链路

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

`/etc/nginx/nginx.conf` 是 mgmtd 生成的（首行写明
`AUTOMATICALLY GENERATED`），关键段落：

```
http {
    proxy_connect_timeout       600;      ← 上游挂住时日志要 600 秒后才出现
    ...
    listen       443 ssl;
    location / {
        proxy_pass http://0.0.0.0:58080;  ← uhttpd，正常
    }
    location ^~ /ws {
        proxy_pass http://0.0.0.0:7681/ws;  ← ttyd，坏
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
    }
}
```

## 破案过程

### 第一步：排除权限

一开始怀疑是权限 —— root 的 `privilege=0`，`check_normal()` 要 ≥3。
但日志推翻了这个判断：`Web login as user is_admin`，
`/tmp/ubmc-web-console-token/` 下确实有 token 文件（08:49、08:55 两个），
说明 `action_console()` 已经成功执行过，页面也渲染了。问题在更下游。

### 第二步：排除「ttyd 没起来」

手工 `ttyd /usr/bin/serial_socket_client` 能正常监听
`NOTICE: Creating Vhost 'default' port 7681`。

### 第三步：决定性实验

**把 ttyd 杀掉，让 7681 上根本没人监听**，然后从外部请求 `/ws`：

```
/ws -> 000  time=6.01s   （挂死）
```

端口关闭时 `connect()` 会立刻收到 RST → nginx 立刻回 502。
**挂死说明包被 DROP 了，不是没人监听。**

### 第四步：找到 DROP 规则

```
iptables -S INPUT

-A INPUT -i vrf-pub -p tcp --dport 58080 -m mac ! --mac-source DE:4F:94:56:CB:C8 -j DROP
-A INPUT -i vrf-ib-vlan -p tcp --dport 58080 -m mac ! --mac-source 46:BA:51:CD:27:34 -j DROP
-A INPUT -i vrf-ib       -p tcp --dport 58080 -m mac ! --mac-source 9A:10:5F:79:AE:1B -j DROP
-A INPUT -i vrf-pub -p tcp --dport 58080 -j ACCEPT     ← 1702 包，nginx→uhttpd 的 hairpin
-A INPUT -i vrf-pub -p tcp --dport 443   -j ACCEPT
-A INPUT -i vrf-pub -p tcp --dport 22    -j ACCEPT
-A INPUT -i vrf-pub -p icmp              -j ACCEPT
-A INPUT -i vrf-pub -m state --state RELATED,ESTABLISHED -j ACCEPT
-A INPUT -i vrf-pub -j DROP                            ← 7681 掉进这里
```

`DE:4F:94:56:CB:C8` 正是 `vrf-pub` 设备自己的 MAC
（`cat /sys/class/net/vrf-pub/address`）。

nginx 在 vrf-pub 里连 `0.0.0.0:7681` 时，流量从 `vrf-pub` 设备出去再绕回来
（hairpin），源 MAC = vrf-pub 自己的 MAC。所以针对 58080 的那条
「源 MAC 不是 vrf-pub 就 DROP」不会误伤它，包继续往下走，命中
`--dport 58080 -j ACCEPT`。

**7681 没有对应的例外，直接落到最后的 `-i vrf-pub -j DROP`。**

### 第五步：为什么只有 58080 有例外

`is_mgmtd` 的字符串里有现成的模板：

```
$ strings /usr/bin/is_mgmtd | grep -B2 -A2 'mac-source'
killall -q uhttpd ttyd
iptables -I INPUT -p tcp --dport %d -i %s -m mac ! --mac-source $(cat /sys/class/net/%s/address) -j DROP
iptables -I INPUT -p tcp --dport %d ! -s 127.0.0.1 -j DROP
/opt/silc-web/silc-web-start.sh %s:%d &
ttyd %s &
```

`is_mgmtd` 启动 web 服务时会调这个模板给 **uhttpd 的 58080** 补例外，
但紧跟着拉起 `ttyd %s &` 时**没有再调一次**。

这是 Silicom 的一个疏漏，不是配置问题 ——
`/etc/product/UBMC/defconfig/vendor_config/ATT/ATT.default_config` 里
只有 `pub-drop` / `pub-state` / `pub-icmp` 三条规则：

```
/config/system/mgmt/iptables-rule/pub-drop/priority,0
/config/system/mgmt/iptables-rule/pub-drop/rule,-I INPUT -i vrf-pub -j DROP
/config/system/mgmt/iptables-rule/pub-state/priority,10
/config/system/mgmt/iptables-rule/pub-state/rule,-I INPUT -i vrf-pub -m state --state RELATED,ESTABLISHED -j ACCEPT
/config/system/mgmt/iptables-rule/pub-icmp/priority,10
/config/system/mgmt/iptables-rule/pub-icmp/rule,-I INPUT -i vrf-pub -p icmp -j ACCEPT
```

`pub-ssh(22)` / `pub-web(443)` / `pub-web-be(58080)` 和那三条 MAC 守卫
是 mgmtd 在运行时补进 netopeer datastore
（`/etc/netopeer/ubmc/datastore.xml`）并生效的。7681 从来没进过这个流程。

## 为什么之前怀疑 VRF 是错的

VRF 确实是这台机器的网络架构（`vrf-ib=100 / vrf-ib-vlan=200 / vrf-pub=300`，
在 `ATT.default_config` 里定义，**是原厂设计**），nginx/sshd/uhttpd 也
确实都绑在各 VRF 上。但：

- 把 ttyd 用 `ip vrf exec vrf-pub` 拉进 VRF 后，**`/ws` 依然挂死**
  —— 证明 VRF 归属不是原因
- ttyd 用**裸绑定**（不加 VRF 包装，就是 mgmtd 的原始启动方式）
  加上防火墙例外后，**`/ws` 立刻 101** —— 证明包装根本不需要

另外 `ss -tln` 对**纯绑定**的 ttyd 也显示 `0.0.0.0%vrf-pub:7681`，
所以不能用 `%vrf-x` 来判断进程是否真在 VRF 里 —— 这一点曾误导过诊断。

## 修复

新增 `etc/init.d/S46is_web_fw`，按 58080 的同一套路补两类例外：

```sh
# 7681（ttyd）：只放行「源 MAC = vrf-pub 自身 MAC」的流量，即 nginx 的
# hairpin；局域网直连 7681 仍然 DROP
iptables -I INPUT 1 -i vrf-pub -p tcp --dport 7681 \
    -m mac ! --mac-source $(cat /sys/class/net/vrf-pub/address) -j DROP
iptables -I INPUT <pub-drop 位置> -i vrf-pub -p tcp --dport 7681 -j ACCEPT

# 80（nginx 的 301 跳转）：只回 301，允许外部访问
iptables -I INPUT <pub-drop 位置> -i vrf-pub -p tcp --dport 80 -j ACCEPT
```

设计要点：

1. **插入位置按 `pub-drop` 的当前行号算**，不用固定 `1` / `2`。
   mgmtd 也是用 `-I INPUT`（插到第 1 位），如果它比脚本晚应用规则，
   固定位置会让 `pub-drop` 反压在我们的 `ACCEPT` 上面。
2. **用 `iptables -C` 判重**，脚本可重复执行。
3. **等 `/tmp/mgmtd_started`** 再动手，保证 mgmtd 的规则已经就位。
4. 提供 `stop` 可完整撤销。
5. 不需要 VRF 包装、不需要改 `is_mgmtd` 二进制、不需要改 nginx 配置。

脚本在 `scripts/rebuild-image.sh` 的「6.5」节内嵌生成，
打包时自动安装并 `sh -n` 校验。

## 验证记录（设备 192.168.31.72，第一版固件）

### A/B 对照

| 状态 | `GET /ws` | `GET http://<bmc>/` |
|---|---|---|
| 无规则（= 出厂状态） | `000`，挂 6.0 秒（bug 复现） | `000`，挂 6.0 秒 |
| 有规则 | `HTTP/1.1 101 Switching Protocols` ✅ | `301 → https://192.168.31.72/` ✅ |

### 完整协议栈（带真实 AuthToken 帧，Python 手搓 WS）

```
TLS: TLSv1.2 ECDHE-RSA-AES256-GCM-SHA384
握手: HTTP/1.1 101 Switching Protocols
    Upgrade: WebSocket
    Sec-WebSocket-Accept: KijYCHqeD7XACTg1s4yRKLEqQhM=
    Sec-WebSocket-Protocol: tty
send: {"AuthToken": "E2ETOKEN1234"}
[op=2] 1/usr/bin/serial_socket_client (ubmc)      ← 窗口标题帧
[op=2] 2{ }                                        ← 终端参数帧
[op=2] 0Press  Ctrl+']' if you want to exit.\r\n  ← 串口客户端输出帧
```

浏览器 → nginx(TLS) → ttyd → `serial_socket_client` → `/dev/ttyS1` 全线贯通。

### 安全边界未变

```
curl http://192.168.31.72:7681/   → 000（5 秒超时，仍被 DROP）
```

局域网直连 7681 依旧不通，只能经 nginx 的 443 走。

### 幂等

连跑两次 `S46is_web_fw start`，`iptables -S INPUT | grep -c 7681` 恒为 2、
`dport 80` 恒为 1。

最终的规则顺序：

```
 1 -P INPUT ACCEPT
 2 -A INPUT -i vrf-pub -p tcp --dport 7681 -m mac ! --mac-source DE:4F:... -j DROP
 3 -A INPUT -i vrf-pub -p tcp --dport 58080 -m mac ! --mac-source DE:4F:... -j DROP
 4 -A INPUT -i vrf-ib-vlan -p tcp --dport 58080 -m mac ! --mac-source 46:BA:... -j DROP
 5 -A INPUT -i vrf-ib -p tcp --dport 58080 -m mac ! --mac-source 9A:10:... -j DROP
 6 -A INPUT -i vrf-pub -p tcp --dport 58080 -j ACCEPT
 7 -A INPUT -i vrf-pub -p tcp --dport 443 -j ACCEPT
 8 -A INPUT -i vrf-pub -p tcp --dport 22 -j ACCEPT
 9 -A INPUT -i vrf-pub -p icmp -j ACCEPT
10 -A INPUT -i vrf-pub -m state --state RELATED,ESTABLISHED -j ACCEPT
11 -A INPUT -i vrf-pub -p tcp --dport 7681 -j ACCEPT   ← 新增
12 -A INPUT -i vrf-pub -p tcp --dport 80 -j ACCEPT     ← 新增
13 -A INPUT -i vrf-pub -j DROP
```

（第 2 行是 7681 的 MAC 守卫，被插到了链首，不影响语义。）

## 排查方法论上的坑（都踩过）

### 1. 在设备内用 `/rpc/sys` 起的 shell 做 curl 自测无效

从该 shell 里 curl，全部返回 `000`：

```
http://192.168.31.72:58080/   → 000     ← uhttpd，外面访问其实正常
https://192.168.31.72/        → 000     ← nginx，外面访问其实正常
ip vrf exec vrf-pub curl ...  → 000     ← 加了 VRF 也一样
ping 127.0.0.1                → 100% 丢包
```

连明知能用的服务都测不通，所以**这个 shell 的出向连接不可信**。
判断服务可用性只能从**外部**测。

### 2. 不要用 `/usr/bin/*` 通配 + `strings` 批量扫描

会跑几百个二进制，把**单线程的 uhttpd 卡住**（RPC exec 正是走 uhttpd 的
Lua CGI），表现为整个 Web UI 无响应 8 秒以上。要用 `strings` 就指定文件。

### 3. `proxy_connect_timeout` 是 600 秒

上游连不上时，`nginx_error.log` 要等 600 秒才写一行。短时间测试看不到
新错误 ≠ 请求没走代理。

### 4. `ss` 的 `%vrf-x` 后缀不能用来判断进程是否在 VRF 里

纯绑定（无 VRF 包装）的 ttyd 也显示 `0.0.0.0%vrf-pub:7681`。

### 5. 「超时」vs「被拒」

`110 ETIMEDOUT` = 包被 DROP；`111 ECONNREFUSED` = 端口没人监听。
把 ttyd 杀掉后仍然超时，这一个观察就锁定了「防火墙」而不是「服务没起」。

## 上游应该怎么修

给 `is_mgmtd` 加一行：拉起 ttyd 之后用同一个模板给 7681 补例外，
或者干脆把 ttyd 也纳入 `vrf-process-list` 的端口例外生成流程。

我们的 `S46is_ttyd_fw` 是在不碰闭源二进制前提下的等效做法。
将来如果能改 mgmtd 源码，这段 init 脚本可以去掉。
