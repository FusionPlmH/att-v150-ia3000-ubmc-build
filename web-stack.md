# uBMC 的 Web/API 栈：LuCI、RPC、REST API、Redfish

这四个东西在 uBMC 上**是四套独立的路由**，容易混为一谈。它们共用同一个
LuCI dispatcher 和同一个 mgmtd 后端，但入口、用途、权限模型各不相同。

```
                        nginx :443
                            │  proxy_pass
                            ▼
                     mgmtd 内建 HTTP :8080
                            │
                            ▼
                  LuCI dispatcher.lua
        ┌───────────────┼───────────────┬──────────────┐
        ▼               ▼               ▼              ▼
   /cgi-bin/sweb     /rpc/*         /restapi/*     /redfish/*
    (Web UI)      (JSON-RPC)      (REST JSON)   (DMTF 标准)
        │               │               │              │
        └───────────────┴───────────────┴──────────────┘
                            │
                            ▼
                      mgmtd 节点树
              /config/*  /status/*  /action/*
```

## 一、Web UI（LuCI）—— `/cgi-bin/sweb/...`

交互式网页，由 `dispatcher.lua` 按 **privilege 等级**路由。
`/cgi-bin/sweb/admin/system/upgrade` 这类路径就属此列，返回 HTML。

判权依据是 mgmtd 用户树里的 `privilege` 字段（1=readonly / 2=normal / 3=admin）。
用户在 mgmtd 树里查不到时 privilege 视为 0 → 页面全部 403。

## 二、RPC —— `/rpc/*`（JSON-RPC）

源码：`base/src/silc_web/modules/rpc/luasrc/controller/rpc.lua`

五个端点：

| 端点 | 暴露的对象 | 能力 |
|---|---|---|
| `/rpc/sys` | 整个 `luci.sys` | **任意命令执行** |
| `/rpc/fs` | `nixio.fs` 的 `readfile`/`writefile` | **任意文件读写** |
| `/rpc/uci` | `luci.model.uci` | 改 UCI 配置 |
| `/rpc/ipkg` | `luci.model.ipkg` | 装/卸软件包 |
| `/rpc/auth` | `luci.jsonrpc` 登录 | 拿 session token |

### ⚠️ 这个模块的权限校验有缺陷

```lua
rpc.sysauth = "root"

local function authenticator(validator, accs)
    local auth = luci.http.formvalue("auth", true)
    if auth then
        local sdat = luci.sauth.read(auth)
        if sdat then
            if sdat.user and luci.util.contains(accs, sdat.user) then
                return sdat.user, auth          -- 只判 sdat.user == "root"
            end
        end
    end
    luci.http.status(403, "Forbidden")
end
```

`accs` 就是 `sysauth` 的值 `"root"`。校验**只判断当前 session 的用户名是不是
root，完全不看 privilege 等级**。

于是形成一条完整的提权链：

1. `root` 在 `/etc/shadow` 里有密码 → 能通过 `checkpasswd`
2. 但 `root` **不在** mgmtd 用户树里 → 正常页面 privilege = 0 → 全部 Forbidden
3. `/rpc/*` 只看用户名 → **畅通**
4. `rpc_sys` 直接把 `luci.sys` 挂到 JSON-RPC 上 → 执行任意命令

这就是 `ia3000-ubmc-access-walkthrough` 里拿 root 用的路子，
入口 URL 是 `https://<bmc>/cgi-bin/sweb/rpc/sys?auth=<sid>`。

**原版固件和自编译版都有这个洞** —— RPC 模块是 `silc_web` 的公开组件，
`base/src/silc_web/modules/rpc/` 在源码树里一直都在。

> 实测（自编译版运行中）：
> ```
> /cgi-bin/sweb/rpc/sys   -> 403   ← 存在，只是未认证
> /cgi-bin/sweb/rpc/fs    -> 403
> /cgi-bin/sweb/rpc/uci   -> 403
> ```

### 想堵掉的话

三个层次，从轻到重：

1. **改校验逻辑**：把 `luci.util.contains(accs, sdat.user)` 换成查 mgmtd 的
   privilege，要求 ≥ 3（admin）
2. **不装这个模块**：从 `silc_web` 的模块列表里去掉 `rpc`
3. **防火墙隔离**：只允许可信网段访问 443

## 三、REST API —— `/restapi/v1/*`

源码：`opt/silc-web/host/usr/lib/lua/luci/restapi.lua` + `restapi/*.lua`，1,305 行。

作用：**把 mgmtd 的节点树映射成 JSON over HTTP 的 REST 接口**。

### URI 结构（`restapi/root.lua`）

```
/restapi/v1/config/system/...       读写系统配置
/restapi/v1/config/accounts/...     读写本地用户 / TACACS / RADIUS
/restapi/v1/status/system/...       读状态
/restapi/v1/action/system/...       执行动作
```

方法：`GET` / `POST` / `PUT` / `PATCH` / `DELETE`（`restapi.lua:250-285`）。

### 可读写的字段（`restapi/system.lua`）

```
management/ethernetInterface/{enabled, ipOrigin, ipAddress, ipMask,
                              ipDefaultGateway, dhcpSendName, dnsServer,
                              ipPermitted, ipPermittedList}
http / https        端口、开关、证书
cliLoginBanner      CLI 登录横幅
cliSessionTimeout   会话超时
datetime            日期时间
deviceName          设备名
event               事件
fileSizeLimit       文件大小限制
```

### 可执行的动作（`action_dict`）

`switchSoftware`、`generateSSHKeys`、`setDate`、`setTime`、
`saveCurrentConfiguration`、`halt`、`reboot`、`reset`、`resetLog`、`clearEvent`

### 用户管理（`restapi/accounts.lua`）

```
/config/unix/user/{fullName, password, privilege}   ← privilege: 1/2/3
/config/tacacs/...   /config/radius/...
```

### 权限模型

走 `http_auth(request)`（dispatcher 里的辅助函数），是正常的认证 + 授权，
没有 RPC 模块那种"只看用户名"的缺陷。

## 四、Redfish —— `/redfish/v1/*`

源码：`opt/silc-web/host/usr/lib/lua/luci/redfish.lua`，488 行。

DMTF 标准（IPMI 的后继者），给上层管理平台用的标准接口。

### 资源树

```
/redfish/v1/                          服务根
/redfish/v1/Systems                    主机信息
/redfish/v1/Managers                   BMC 自身
/redfish/v1/Chassis                    机箱、传感器
/redfish/v1/AccountService/Roles/1     角色
/redfish/v1/AccountService/Accounts/1  账户
/redfish/v1/SessionService             会话
```

响应带 `@odata.id`、`@odata.count`、`@odata.context`、`@odata.type` 等标准字段。

方法：`GET`（`handle_get`）、`POST`（`handle_post`）、`PATCH`（`handle_patch`）、
`DELETE`（`handle_delete`）。

### 用户操作直接落到 mgmtd

```lua
mgmtdclient.add("/config/unix/user/"..UserName, UserName)
mgmtdclient.modify("/config/unix/user/"..UserName.."/passwd", Password)
mgmtdclient.delete("/config/unix/user/"..key)
```

**用途**：Ansible `redfish` 模块、OpenStack sushy、VMware、各类 DCIM —
不用学厂商私有接口就能纳管。

## 五、REST/Redfish 影响面

这台机器所有**交互式**访问都不经过 REST/Redfish：

| 通道 | 路径 | 受影响 |
|---|---|---|
| Web UI | `/cgi-bin/sweb/admin/...`（LuCI）| ❌ |
| IPMI | SSIF / i2c | ❌ |
| SNMP | net-snmp | ❌ |
| CLI | 串口 / SSH → mgmtd | ❌ |
| RPC | `/rpc/*` | ❌ |
| **程序化 API** | `/restapi`、`/redfish` | ✅ **仅此一项** |

所以缺的只是「给自动化平台用的标准接口」，不影响任何人工操作。

## 六、自编译版的实际状态（第四轮已补齐）

| 组件 | 原版 1.2.0.8 | 自编译（第四轮） |
|---|---|---|
| Web UI (LuCI) | ✅ | ✅ |
| RPC (`/rpc/*`) | ✅ **有提权洞** | ✅ **同样有提权洞** |
| REST API (`/restapi`) | ❌ 文件在但路由没接、且被 product.lua 关掉 | ✅ **200** |
| Redfish (`/redfish`) | ❌ 文件在但路由没接 | ✅ **200** |

公开源码 `silicom-ltd/uBMC` 里就没有 `restapi.lua` / `redfish.lua`，
属于厂家内部新增。

### 接通过程踩到的两个坑

**坑一：路由从来没接上。**

出厂的 `www/restapi` 是个 169 字节的 CGI 脚本（`dofile setup.lua` +
`luci.sgi.cgi.run()`），放在 docroot 根目录。但 uhttpd 的 CGI 前缀默认
只有 `/cgi-bin`，启动命令也没带 `-x`。实测在 docroot 根放一个可执行脚本：

```
GET /zztestcgi  ->  200, Content-Type: application/octet-stream
                    返回的是脚本源码本身
```

**不会被当 CGI 执行，只会被当静态文件下载。** 所以 `/restapi` 一直是死的。

**坑二：上游把 REST 自己关了。**

`restapi/product.lua`：

```lua
function customize(root_dict)
    -- disable restapi for other vendors
    --if string.find(vendor, 'SILICOM') == nil then
        for k, v in pairs(root_dict) do root_dict[k] = nil end
        return
    --end
end
```

那个 `if` 被注释掉了，于是 `customize()` **无条件清空整棵 REST 树**，
每个请求都是 404。而 `/etc/product/vendor` 在本机是空的 —— 就算把判断
恢复，`SILICOM` 也匹配不上，照样关掉。

### 我们的做法

1. dispatcher **按路径首段分流**（`r[1] == "restapi"/"redfish"`），
   比原厂按 `REQUEST_URI` 前缀判断更稳：无论从 `/restapi/...` 还是
   `/cgi-bin/sweb/restapi/...` 进来都能工作。
2. `http_auth()` 用 HTTP Basic + PAM（重构出 `pam_auth()` 复用）。
3. 顶层 `require` 用 `pcall` 包起来 —— 缺 `cjson` 这类可选依赖时，
   只让 API 不可用，不会拖垮整个 Web UI（这个坑真踩过）。
4. `S46is_web_fw` 在 mgmtd 生成的 `nginx.conf` 里补两条 location：

```nginx
location ^~ /restapi {
    proxy_pass http://0.0.0.0:58080/cgi-bin/sweb/restapi;
}
location ^~ /redfish {
    proxy_pass http://0.0.0.0:58080/cgi-bin/sweb/redfish;
}
```

改完先 `nginx -t`，通过才 `kill -HUP` reload。

5. `http/protocol.lua` 换成出厂版（补 PATCH/PUT/DELETE 的 body 解析
   和 401 状态码）。

### 实测

```
GET /restapi/v1/config/system   无认证 -> 401（带 WWW-Authenticate）
GET /restapi/v1/config/system   认证后 -> 200，完整系统配置 JSON
GET /restapi/v1/status/system   认证后 -> 200，会话列表
GET /restapi/v1                 认证后 -> 403（顶层不允许访问）
GET /redfish/v1                 认证后 -> 200，标准 ServiceRoot
GET /redfish/v1/Manager         认证后 -> 200，含真实 Datetime
GET /redfish/v1/Chassis/Members/0/Power -> 200
```

认证：`curl -k -u is_admin:<password> https://<bmc>/redfish/v1`
