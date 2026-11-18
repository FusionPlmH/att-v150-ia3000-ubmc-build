# 自编译 uBMC vs 出厂 1.2.0.8 —— 完整对比

对比对象：

| | 来源 | 版本 |
|---|---|---|
| **原版** | eMMC p2（`SYSTEM1`）解出的 rootfs | `UBMC_1.2.0.8_BUILD_1d22f81a` |
| **自编译** | 公开源码 `silicom-ltd/uBMC` @ `bd9de8c` 构建 | `UBMC_1.1.0.5_BETA_BUILD_bd9de8c` |

> 两者源码谱系**互不为祖先**：出厂 `1d22f81a` 从未公开，公开仓库 HEAD 停在
> 2020-10 的 `464758c`，fork 在 2022-05 的 `bd9de8c`。

---

## 一、规模

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

## 三、内核与内核模块

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

## 五、原版有、自编译没有的 110 项

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

公开源码里根本没有这部分。**这既是功能缺失，也是安全优势** ——
我们当初就是走 REST 的 `/rpc/sys` 拿到 root 的；自编译版没有这个面。

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

### 有差距的（真实差异）

| 影响 | 项 |
|---|---|
| 🔴 **功能缺失** | REST API / Redfish 整块（21 项）—— 但也是安全优势 |
| 🟡 **版本落后** | OpenSSL 无 1.1.1、glib −1 小版本、net-snmp 单版本、libcurl/libssh 旧 |
| 🟡 **源码落后** | 1.1.0.5-BETA vs 1.2.0.8，出厂版是内部分支，**无法从公开源追平** |
| 🟢 平台适配 | `S35is_ipmi` 缺 SKY-D-CC 分支（本机用不到） |
| ✅ 已修正 | sshd 家目录已改回 `/var/empty`，`/etc/passwd` 除 root 行外与出厂一致 |

### 判断

自编译版**功能上是原版的子集**，核心服务（IPMI、SNMP、LuCI、nginx、sshd、
netopeer、dbus、交换机管理）一个不缺，内核与驱动几乎完全一致。

差距集中在：
1. **REST/Redfish 栈**（源码里就没有）
2. **第三方库的次要版本**

如果要追平，只能拿到 `1d22f81a` 那份源码 —— 而它没有公开。
