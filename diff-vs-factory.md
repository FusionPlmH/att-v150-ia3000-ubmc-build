# 自编译 uBMC vs 出厂 1.2.0.8 —— 完整对比

> ## ⚠️ 这是**第二 / 第四轮的历史快照**，不是现状
>
> 当时的对比对象是"**纯自编译镜像**（`bd9de8c` + Buildroot 2018.02）" vs "出厂 1.2.0.8"。
> 之后又做了二十多轮，bank1 上跑的东西早就不是那个"纯自编译"版本了
> （Web 栈改造、自建 IPMI 服务端、防火墙规则、静态链接……）。
> **逐轮增量请看 `ubmc-bank1/MANIFEST.txt`**（在本地工作目录里，不在本仓库）—— 那里每轮都有
> 镜像大小、sha256、版本号和变更清单。
>
> ### 现状实测（第二十五轮，同一把尺子：直接读 p2 / p3 里的 `rootfs.cpio.uboot`）
>
> | 指标 | 出厂（`/dev/mmcblk0p2`） | 现在（`/dev/mmcblk0p3`，v25） |
> |---|---|---|
> | 镜像字节 | 53,247,113 | **55,823,961** |
> | 普通文件字节和 | 147,126,340（147.1 MB） | 150,292,842（150.3 MB） |
> | `cpio -t` 条目 | 7,925（目录 967 / 链接 550 / 文件 6,408） | 7,885（目录 953 / 链接 544 / 文件 6,388） |
> | 版本横幅 | `##UBMC_1.2.0.8_BUILD_1d22f81a##` | `##UBMC_1.1.0.5_LOCAL_BUILD_bd9de8c_v25##` |
>
> 出厂的字节数和下面第一节的 147.1 MB **完全对得上**，说明是同一份镜像；
> 条目数差 411 只是**计数口径不同**（同一份镜像，字节数一致）。
>
> **结论已经反过来了**：第二节说"自编译比出厂小 18 MB"——那只是纯 Buildroot 产物的结果。
> 现在这一版比出厂**大 3.2 MB**（多了 ttyd/luci/nginx 改造、自建 IPMI 守护进程等）。
> 内核侧没变差：`zImage` 仍是同源码同配置编出来的（**头部逐字节相同、内容哈希不同**，
> 因为构建时间不同），`dt.dtb` 与出厂 **sha256 完全相同**。详见第三节。

对比对象：

| | 来源 | 版本 |
|---|---|---|
| **原版** | eMMC p2（`SYSTEM1`）解出的 rootfs | `UBMC_1.2.0.8_BUILD_1d22f81a` |
| **自编译** | 公开源码 `silicom-ltd/uBMC` @ `bd9de8c` 构建 | `UBMC_1.1.0.5_BETA_BUILD_bd9de8c` |

> 两者源码谱系**互不为祖先**：出厂 `1d22f81a` 从未公开，公开仓库 HEAD 停在
> 2020-10 的 `464758c`，fork 在 2022-05 的 `bd9de8c`。

---

## 一、规模（第二轮快照）

| 指标 | 原版 | 自编译 | 差 |
|---|---|---|---|
| 条目总数 | 8,336 | 8,117 | −219 |
| 普通文件 | 6,818 | 6,629 | −189 |
| 目录 | 967 | 949 | −18 |
| 符号链接 | 550 | 538 | −12 |
| 解包后总字节 | 147.1 MB | 129.0 MB | **−18.1 MB** |
| `rootfs.cpio.uboot` | 53.2 MB | 46.2 MB | −7.0 MB |

顶层目录结构完全一致（`bin dev etc init lib lib32 linuxrc media mnt opt proc root run sbin sys tmp usr var`）。

## 二、文件级差异

`diff -rq` 原始输出 3,168 行，但要分层看：

| 类别 | 数量 |
|---|---|
| 只在原版有 | 110 |
| 只在本版有 | 12 |
| 内容不同（原始） | 3,045 |
| ├ 实为符号链接误报（`cmp` 跟随链接） | 652 |
| ├ **只差 ELF build-id / .pyc 时间戳** | **1,726** |
| └ **真正内容不同** | **667** |

> 「只差 build-id」的判定方法：把 `.note.gnu.build-id` 的 20 字节 SHA1
> 描述符和 `.pyc` 头里的 mtime/size 字段清零后逐字节比较。
> 3,045 里 2,378 项（78%）是构建元数据，不是代码差异。

### 667 项真正差异的分布

| 路径 | 数量 | 说明 |
|---|---|---|
| `usr/lib` / `usr/lib32` | 158 / 158 | 库版本漂移（同一文件硬链接算一处） |
| `usr/bin` | 150 | busybox 之外的工具 |
| `lib/security` / `lib32/security` | 42 / 42 | PAM 模块 |
| `opt/silc-web` | 24 | LuCI 框架与页面 |
| `usr/sbin` | 16 | 服务程序 |
| `usr/share` | 13 | MIB、Python |
| `usr/libexec` | 11 | |
| `lib/udev` / `lib32/udev` | 5 / 5 | |
| `etc` | 约 10 | 见第四节 |
| `lib/modules` | **1** | 见第三节 |

## 三、内核与内核模块（**结论至今有效**：同源码、同配置，只有 1 个 .ko 有差异）

| 项 | 结果 |
|---|---|
| zImage 大小 | **两边都是 4,211,200 B** |
| zImage 头 | `MZ`×4 EFI PE stub + ARM magic `0x016f2818` + `PE\0\0`，**逐字节相同** |
| zImage 内容 | 哈希不同（构建时间不同） |
| `dt.dtb` | **sha256 完全相同**（`23c76bca…`） |
| 内核模块数量 | **316 / 316** |
| 内容不同的 `.ko` | **仅 1 个** |

唯一不同的模块：

```
lib/modules/4.14.54/kernel/drivers/i2c/busses/ubmc-ipmi-slave.ko
    出厂 51,112 B   vs   自编译 51,240 B   (+128 B)
```

这是 **IPMI-over-i2c（SSIF）从机驱动** —— 正是我们摸 IPMI 用的那个通道。
其余 315 个模块（网络、文件系统、MMC、i2c 等）二进制完全相同。

**结论：内核配置基本一致，只有这一个驱动有源码级差异。**

## 四、配置与脚本差异

### `/etc/passwd` —— 只差一行

```diff
- sshd:x:1001:1002:SSH drop priv user:/var/empty:/bin/false     ← 出厂
+ sshd:x:1001:1002:SSH drop priv user:/:/bin/false             ← 自编译
```

出厂给 sshd 特权分离用户用了独立家目录 `/var/empty`，自编译是 `/`。
**属于安全加固上的小退步** —— ✅ **已在最终固件里修正**：把 `/etc/passwd`
改回 `:/var/empty:/bin/false`，现在除 root 一行外与出厂 `/etc/passwd` 完全一致。

### `/etc/group` —— **完全相同**

### `/etc/shadow` —— 只差 root 密码哈希
两边其余账户（`daemon`/`bin`/`sys`/`sync`/`mail`/`www-data`/`operator`/`nobody`/`dbus`/`sshd`/`tss`）**全部是 `*` 锁定**。

### `/etc/init.d` —— 只有 3 个脚本不同

```diff
# S35is_ipmi
+			"SKY-D-CC")                 ← 出厂多一个平台分支
+			load_driver_for_skyd

# S50nginx
-	mkdir -p /var/log/nginx /var/cache/nginx      ← 出厂
+	mkdir -p /var/log/nginx /var/tmp/nginx        ← 自编译

# S59snmpd
+ test -x /usr/sbin/snmpd || exit 0              ← 自编译多了存在性保护
+ test -x /usr/sbin/snmptrapd || exit 0
```

`S35is_ipmi` 的差异是**平台适配**（出厂支持 Keysight SKY-D-CC 板），与本机无关。

### 版本文件

| 文件 | 出厂 | 自编译 |
|---|---|---|
| `software_version.txt` | `##UBMC_1.2.0.8_BUILD_1d22f81a##` | `##UBMC_1.1.0.5_BETA_BUILD_bd9de8c##` |
| `firmware_version.txt` | `##UBMC_FW_…` | `##UBMC_FW_0.9.5_BUILD_bd9de8c##` |
| `uboot_version.txt` | `##UBMC_UBOOT_1.0.2_1d22f81a##` | `##UBMC_UBOOT_1.0.2_bd9de8c##` |
| `dtb_version.txt` | — | `##UBMC_DTB_0.9.5_bd9de8c##` |

## 五、原版有、自编译没有的 110 项（第二轮口径；见文首现状说明）

> ### 这 110 项基本都补回来了 —— 第二~四轮"从出厂移植"就是干这个的
>
> 后来的做法是：**不重编 Buildroot，直接把出厂固件里同 SoC、同内核版本的二进制与库
> 搬进自编译镜像**（ABI 完全兼容），见 `ubmc-bank1/MANIFEST.txt`
> 的「第二轮升级」「批次一」。现在设备上实测：
>
> | 原缺项 | 现状 |
> |---|---|
> | ① REST API / Redfish（`restapi.lua` `redfish.lua` `restapi/*.lua`） | ✅ 已移植（`/opt/silc-web/host/luci/` 里都有） |
> | ① `cjson.so`、`json2lua`/`lua2json`、`/usr/share/lua/5.1/*` | ✅ 已移植（`/usr/lib/lua/5.1/cjson.so`） |
> | ② valgrind 全家桶 | ✅ 已移植（`/usr/bin/valgrind` 与出厂**逐字节相同**，sha256 `7dd0a60c…`） |
> | ③ CREANORD 主题 / vendor_config | ❌ 有意不移植（本机 `Vendor: ATT`） |
> | ④ `etc/phonehome`、`S99seth` | ❌ 有意不移植（Keysight 专用） |
> | ⑤ OpenSSL 1.1.1f | ✅ 已移植，`libcrypto.so.1.1` 与 1.0.0 并存 |
> | ⑤ net-snmp 5.9 | ✅ 已移植，`libnetsnmp.so.30` + `.40` 两套并存 |
> | ⑤ libcurl 4.6.0 / libssh 4.8.6 / glib 2.54.3 | ✅ 已移植（旧的 4.5.0 / 4.4.2 / 2.54.2 也留着） |
> | ⑥ `ntpq`、`snmp*` 全套、`ssh-copy-id` | ✅ 已移植 |
> | ⑥ `/usr/lib/tmpfiles.d`、`bmc-fan.lua` | ❌ 仍缺（无人使用） |
>
> 这就是现在镜像比出厂**大 3.2 MB** 的原因。下面原始清单保留作为对照。

### ① REST API / Redfish（**最重要的功能性缺失**）

```
opt/silc-web/host/luci/restapi.lua
opt/silc-web/host/luci/redfish.lua
opt/silc-web/host/luci/restapi/          (目录)
opt/silc-web/host/www/restapi/           (目录)
usr/lib/lua/5.1/cjson.so
usr/share/lua/5.1/cjson, json2lua.lua, lua2json.lua
usr/bin/json2lua, lua2json
```

公开源码里根本没有这部分。它只影响**程序化 API 访问** ——
Web UI（`/cgi-bin/sweb/admin/...`）、IPMI（SSIF）、SNMP、CLI 全都不经过它。

> ⚠️ **更正**：早期版本的本节曾写作「REST API 是我们拿 root 的入口，
> 缺了反而是安全优势」—— **这是错的**。
> 拿 root 走的是 `/rpc/sys`，属于独立的 **RPC 模块**
> （`base/src/silc_web/modules/rpc/`），与 REST API 无关。
> 该模块在自编译版里**同样存在**，详见
> [web-stack.md](web-stack.md#rpc-模块与-restapiredfish-的区别)。

### ② valgrind 全家桶（~20 个）

```
usr/bin/valgrind valgrind-di-server valgrind-listener vgdb
        callgrind_annotate callgrind_control cg_annotate cg_diff cg_merge
        ms_print
usr/lib/valgrind/          (目录)
```

调试工具，生产不需要。

### ③ CREANORD 厂商定制

```
opt/silc-web/host/etc/config/CREANORD.luci
opt/silc-web/host/luci/view/themes/creanord
opt/silc-web/host/www/luci-static/creanord
opt/silc-web/host/etc/uci-defaults/luci-theme-creanord
etc/product/UBMC/defconfig/vendor_config/CREANORD/
```

本机 `Vendor: ATT`，不涉及。

### ④ phonehome / Keysight

```
etc/phonehome
etc/init.d/S99seth        ← Keysight 专用
```

### ⑤ 库版本差异（结构性）

| 组件 | 出厂 | 自编译 |
|---|---|---|
| OpenSSL | `libcrypto.so.1.0.0` **+** `libcrypto.so.1.1` | 只有 `1.0.0` |
| libcurl | `libcurl.so.4.6.0` | 旧版 |
| libssh | `libssh.so.4.8.6` | 旧版 |
| net-snmp | `libnetsnmp.so.35` **和** `.40`（两套并存） | 只有一套 |
| glib | 2.54.**3** (`…0.5400.3`) | 2.54.**2** (`…0.5400.2`) |

> OpenSSL 只有 1.0.0 **不是故障** —— nginx/sshd/curl 全部链接 `.so.1.0.0`，
> 构建自洽，能跑。

### ⑥ 其他工具

`ntpq`、`snmpping`/`snmppcap`/`snmpps`/`snmptop`、`ssh-copy-id`、`ssh-sk-helper`、
`usr/lib/tmpfiles.d`、`usr/lib64/sa`、`bmc-fan.lua`、`hw_config`、
`etc/ssl/ct_log_list.cnf`、`openssl.cnf.dist`

## 六、自编译有、原版没有的 12 项

```
usr/lib{,32}/libgio-2.0.so.0.5400.2      glib 2.54.2（原版是 .3）
usr/lib{,32}/libglib-2.0.so.0.5400.2
usr/lib{,32}/libgmodule-2.0.so.0.5400.2
usr/lib{,32}/libgobject-2.0.so.0.5400.2
usr/lib{,32}/libgthread-2.0.so.0.5400.2
usr/lib{,32}/sa                           sysstat 装在 /usr/lib 而非 /usr/lib64
```

全部是**版本号/路径不同**，不是新增功能。

---

## 七、结论

### 一致的（可以放心）

- `/etc/group` 完全相同
- `/etc/shadow` 除 root 外全部一致（都是锁定）
- **316 个内核模块里 315 个完全相同**
- `dt.dtb` 逐字节相同
- zImage 大小与头部结构完全相同
- init.d 只有 3 个脚本有差异，且都不影响本机
- 98% 的文件只有构建元数据差异

### 有差距的（真实差异 —— **第二轮口径，现状见下面「判断」**）

| 影响 | 项 |
|---|---|
| 🔴 **功能缺失** | REST API / Redfish 整块（21 项）—— 仅影响程序化 API，不影响任何交互式功能 |
| 🟡 **版本落后** | OpenSSL 无 1.1.1、glib −1 小版本、net-snmp 单版本、libcurl/libssh 旧 |
| 🟡 **源码落后** | 1.1.0.5-BETA vs 1.2.0.8，出厂版是内部分支，**无法从公开源追平** |
| 🟢 平台适配 | `S35is_ipmi` 缺 SKY-D-CC 分支（本机用不到） |
| ✅ 已修正 | sshd 家目录已改回 `/var/empty`，`/etc/passwd` 除 root 行外与出厂一致 |
| ⚠️ 共有风险 | `/rpc/sys` 任意命令执行洞 —— **原版和自编译版都有**，源自共享的 RPC 模块 |

### 判断

> **第二轮结论**：自编译版功能上是原版的子集。
> **现状（第二十五轮）**：已经不是子集了 —— REST/Redfish、valgrind、net-snmp 5.9、
> OpenSSL 1.1.1f、nginx 1.20.2、OpenSSH 8.2p1 全部从出厂移植进来，
> 而原版没有的（自建 IPMI over LAN 服务端、SOL、PET、WebTTY 修复）是我们加的。
> 两边现在是**各自的超集**，差别只在厂商定制件（CREANORD / phonehome / Keysight）。
>
> 核心服务（IPMI、SNMP、LuCI、nginx、sshd、netopeer、dbus、交换机管理）一个不缺，
> 内核与驱动仍与出厂逐字节一致。

差距集中在：
1. **REST/Redfish 栈**（公开源码里没有，只能从出厂映像里搬文件）
2. **第三方库的次要版本**（现在基本追平了）
3. **厂商定制件**（CREANORD / phonehome / Keysight —— 本机用不到，不移植）

如果要真正从源码追平，只能拿到 `1d22f81a` 那份源码 —— 而它没有公开。
