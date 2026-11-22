# uBMC 的谱系：它不是 OpenBMC 的魔改

**结论：Silicom uBMC 与 [openbmc/openbmc](https://github.com/openbmc/openbmc) 没有代码上的继承关系。**

两者的「BMC」只是同一个词的两个不同实现方向。之所以容易误会，
是因为 uBMC 的 README 里写着 "open source remote management platform"，
而 OpenBMC 是「BMC 界的开源」代名词 —— 但那是两码事。

---

## 决定性证据

### ① 构建系统不是同一套（这条就够定案）

| | openbmc | Silicom uBMC |
|---|---|---|
| 构建系统 | **Yocto / OpenEmbedded** | **Buildroot 2018.02** |
| 顶层标志物 | `bitbake`→、`meta`→、`meta-openembedded`→、`meta-phosphor/`、`oe-init-build-env`、`openbmc-env`、`meta-<厂商>/` | `base/`、`buildroot/`、`product/`、`scripts/`、`patch_config/` |
| 版本变量 | Yocto 发行版版本 | `export BR2_VERSION := 2018.02` |

**OpenBMC 从来不是 Buildroot 项目。** 它从 2015 年起就是 Yocto 下的
`meta-phosphor` 层集合。一个 Buildroot 工程不可能是它的下游。

### ② 运行时栈完全不同

在出厂 rootfs（7,368 文件）里逐项查证：

| 特征 | openbmc | 出厂 uBMC |
|---|---|---|
| D-Bus 命名空间 | `/xyz/openbmc_project`（招牌） | **不存在，连 `/xyz` 目录都没有** |
| init | systemd | **busybox init**（`/sbin/init` → `busybox`，`/etc/init.d/S??` + `rcS`/`rcK`） |
| Redfish | `bmcweb`（C++） | **`redfish.lua` / `restapi.lua`**（Lua） |
| IPMI | `phosphor-ipmi-host` + `phosphor-net-ipmid` | **自研最小实现** |
| 传感器 | `entity-manager` + `dbus-sensors` + `phosphor-hwmon` | 直接读 `/sys/class/hwmon` + 内置表 |
| Web UI | `webui-vue` / `phosphor-webui` | **LuCI**（`/opt/silc-web/host/usr/lib/lua/luci/`）+ nginx |
| 日志 | `phosphor-logging` + journald | busybox syslogd |

`bmcweb`、`phosphor-*`、`entity-manager` 在出厂 rootfs 里**一个都找不到**。

### ③ 源码搜索为零

```sh
grep -ri openbmc  /root/uBMC-fork/          # 0 处
git log  https://github.com/silicom-ltd/uBMC # 全部是 ssh://file/git/uBMC 内部仓库
```

GitHub 代码搜索 `repo:silicom-ltd/uBMC phosphor` → **0 结果**。
`phosphor` 在整棵树里唯一的出现是内核 `Documentation/fb/framebuffer.txt`
（讲显示器的荧光粉点距，跟 OpenBMC 无关）。

### ④ IPMI 实现是自写的最小集

如果是从 OpenBMC 改的，IPMI 应该是完整的 phosphor 栈。实测**原厂固件**：

```
厂商 ID 0x030201            ← 「没申请厂商号」的占位值
产品 ID 0x0403
GUID 01 02 03 … 10          ← 同样是占位
实现 NetFn：App 0x01/0x08/0x2F/0x31/0x42/0x57、Storage 0x20-0x49、
            Transport 0x02、Group 0x2C/0x00
原厂未实现：Chassis 全部、S/E 全部、OEM 全部、App 0x43-0x49（用户管理）、
            Set LAN 的写、SOL
```

真正基于 phosphor 的 BMC 不会缺 Chassis 和用户管理。

> 上面这些缺的部分，本项目后来在 **LAN 侧**自己补了一个 IPMI 2.0 服务端
> （`ubmc_ipmi_lan`），现在 chassis 控制 / 用户管理 / SOL / PEF 告警 / SDR 仓库
> 都能用，`ipmitool -I lanplus` 直连即可。原厂那条 KCS/SSIF 通道本身没有改动。
> 详见 [ipmi-lan.md](ipmi-lan.md)。

详见 [ipmi-map.md](https://github.com/FusionPlmH/ia3000-ubmc-access-walkthrough/blob/main/ipmi-map.md)。

## 它到底是什么

- **自研的 Buildroot 2018.02 嵌入式 Linux**，Apache-2.0
- Web 栈借自 **OpenWrt/LEDE**（LuCI + nginx + Lua）—— 这是它唯一明显的「外部祖先」
- IPMI 是自写的最小实现
- SoC：**TI AM3352**（单核 Cortex-A7，`Generic AM33XX`），512 MB RAM
- 存储：7.45 GiB eMMC，8 个分区，A/B 双槽
- git 历史全是 Silicom 内部（`ssh://file/git/uBMC`），公开可见提交最早 2020-10

## 顺带：为什么它也不可能「直接换成 OpenBMC」

AM3352 是通用应用处理器，不是 BMC SoC：

- 没有 ASPEED AST2x00 那一套 **LPC/eSPI 旁路**（KVM、virtual media、POST code 全靠它）
- 没有 VGA 输出
- 单核 600 MHz + 512 MB，跑 `bmcweb` + `phosphor-ipmi-host` + `entity-manager` 很勉强
- 最关键的：**这套硬件的 BMC 功能是围绕 NC-SI + 自研 RPC 设计的**，
  不是围绕 LPC 设计的

所以 OpenBMC 在这块板子上不是一个可替换项，只可能是另起炉灶的重写。

## 关于「Oct 12, 2019」

找不到任何 openbmc ↔ uBMC 的关联时间点。

- 出厂 rootfs 里那批脚本的 `Oct 12 2021` 时间戳，是 Silicom 内部提交日期
- 无论取 openbmc 的哪个时间点（2019-10 还是现在），结论都一样：
  buildroot 工程 ≠ Yocto 工程，不可能是 fork
