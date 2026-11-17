# uBMC 构建

**目标：把 Silicom IA3000 / AT&T ATT-V150 的管理卡（uBMC）固件从源码重新编译出来。**

编完你拿到一组镜像文件，然后可以：

- **先在现跑的 BMC 上 chroot 验证** —— 零风险，不重启、不动 eMMC 引导 ← 推荐
- 确认改动没问题，再决定要不要刷写

这一节讲的是 **BMC 部分**（TI AM3352），和主机侧的 Vyatta 系统无关。

| 项 | 值 |
|---|---|
| BMC 主控 | TI **AM3352BZCZ60**（ARM Cortex-A8 @600 MHz，armv7l / armhf） |
| BMC DDR3 | 512 MB（K4B4G16） |
| 启动介质 | Winbond SPI Flash（U47）+ 板载 eMMC |
| 源码 | <https://github.com/silicom-ltd/uBMC> |
| 出厂固件 | `UBMC_R102_20181205` |
| 串口 | `115200 8N1`，3.3V TTL，前面板 RJ45 console 口 |

> ⚠️ **构建是安全的，刷写才是危险的。**
> 想验证自己的改动，用 [chroot-and-recovery.md](chroot-and-recovery.md) 里的 chroot 法，
> 零风险、不用重启、不动 eMMC 引导。

---

## 你会得到什么

`output/images/` 下五个文件：

| 文件 | 大小 | 说明 |
|---|---|---|
| `zImage` | ~4.2 MB | ARM 内核 |
| `rootfs.cpio.uboot` | — | initramfs（正式启动用它，`root=/dev/ram0`） |
| `rootfs.ext2` | — | 同一个 rootfs 的磁盘镜像 |
| `rootfs.tar` | — | 打包形式（chroot 测试用这个） |
| `dt.dtb` | ~32 KB | 设备树 |

**注意 rootfs 是 cpio ramdisk**（`root=/dev/ram0`），不是分区上的文件系统。
所以**不能用 `ext4write` 直接改 `/etc/shadow`** —— `/etc` 每次启动都从 cpio 重建。

---

## 第 0 步：准备一台构建机

**任何 x86_64 Linux 都行** —— 物理机、PVE 里的 VM、WSL 都可以。内核版本无所谓，
编译在 chroot 里做。

| 项 | 要求 | 自己检查 |
|---|---|---|
| 系统 | Debian 12+ / Ubuntu 22.04+（任何有 `debootstrap` 的） | `cat /etc/os-release` |
| 权限 | root（要 debootstrap + mount） | `id -u` 输出 `0` |
| 磁盘 | **至少 20 GB** 空闲 | `df -h $HOME` |
| 内存 | 2 GB 以上 | `free -m` |
| 网络 | 能访问 `github.com` 和 `archive.debian.org` | `curl -sI https://archive.debian.org` |
| 串口线 | USB-TTL 3.3V（**只在要调试时才需要**） | — |
| 宿主机 | 只有要在真机上验证时才需要 | — |

构建本身**不需要**接串口、不需要那台 uCPE 在旁边、不需要 BMC 在线。
只有最后一步"拿到 BMC 上试"才需要。

---

## 三步构建

### 第 1 步：取源码

```sh
git clone https://github.com/silicom-ltd/uBMC ~/uBMC
```

约 1 GB。**预期输出**：`Receiving objects: 100%` 之后 `~/uBMC/buildroot/` 存在。

```sh
ls ~/uBMC/buildroot/Makefile && echo ok
```

不是 `ok` 就说明克隆没完成。

### 第 2 步：建 chroot

```sh
cd ubmc-build

# 先体检，不改任何东西
./scripts/00-setup.sh --check

# 建 chroot + 装 20 个构建依赖（首次 5~10 分钟，要 root）
sudo ./scripts/00-setup.sh
```

**预期输出**（最后几行）：

```
  Debian         9.13
  gcc            gcc (Debian 6.3.0-18+deb9u1) 6.3.0
  make           GNU Make 4.1
  dtc            Version: DTC 1.4.1
  源码可见        buildroot product ...
```

`Debian` 必须是 **9.x** —— 用新发行版编出来的东西在 BMC 上跑不了
（`GLIBC_2.28 not found`，BMC 上是 glibc 2.24）。

**失败**：`debootstrap` 装不上 → `apt install debootstrap`；
源码路径不对 → `UBMC_SRC=/your/path sudo -E ./scripts/00-setup.sh`

### 第 3 步：编译

```sh
sudo ./scripts/01-build.sh
```

首次约 **20~40 分钟**（取决于 CPU）。Buildroot 是增量的 —— 中断了重跑同一条命令，
已编好的不会重编。

**成功的样子**：

```
=== build exited rc=0 at 2026-11-18T01:23:45+08:00 ===

=== 产出物 ===
-rw-r--r-- 1 root root  4.2M zImage
-rw-r--r-- 1 root root   32K dt.dtb
-rw-r--r-- 1 root root   ...
```

完整日志在 `~/ubmc-work/build.log`。

**失败**：见 [失败了看这里](#失败了看这里)。

### 换路径 / 换镜像源

```sh
# 临时改一次
UBMC_SRC=/data/uBMC UBMC_CHROOT=/data/stretch sudo -E ./scripts/00-setup.sh

# 或者长期改
cp scripts/env.sh scripts/env.local.sh
$EDITOR scripts/env.local.sh        # 改完所有脚本自动生效
```

---

## 编完之后怎么用

三条路，风险从低到高。**先走 A。**

### A. chroot 进现跑的 BMC 上测（推荐，零风险）

不用重启、不动 eMMC 引导、刷坏了也影响不到。见
[chroot-and-recovery.md](chroot-and-recovery.md)。

大致的流程：

```sh
# 1. 把 rootfs 打包，传到 BMC 上（BMC 上有 4.5 G 的 /var/images）
tar czf rootfs.tgz -C ~/uBMC/buildroot/output/images rootfs.tar
scp rootfs.tgz root@<bmc-ip>:/var/images/

# 2. 在 BMC 上解开
ssh root@<bmc-ip>
mkdir -p /var/images/test && tar xf /var/images/rootfs.tgz -C /var/images/test

# 3. chroot 进去跑你要测的东西
/var/images/ubmc-chroot.sh /var/images/test "uname -a; ls /etc"
```

`ubmc-chroot.sh` 在 [scripts/ubmc-chroot.sh](scripts/ubmc-chroot.sh)。

### B. 写 SD 安装盘

**实测走不通** —— 原因和三条证据在 [sdcard.md](sdcard.md)。
AM335x 的 ROM 按 SYSBOOT 引脚决定去哪找 MLO，这台机器只选了 eMMC。

### C. 刷 eMMC

风险最高。改 `boot0.scr` 之前**必须先备份**：

```sh
cp /mnt/p1/boot0.scr /mnt/p1/boot0.scr.orig
```

详见 [chroot-and-recovery.md](chroot-and-recovery.md) 的串口救砖一节。

---

## 失败了看这里

| 卡在哪 | 下一步 |
|---|---|
| `01-build.sh` 报 libfdt / `fdt_overlay.c` | [buildroot.md](buildroot.md) 的 `HOSTCFLAGS` 一节，删掉 `linux.mk` 里那一行再重跑 |
| `01-build.sh` 报 u-boot 源码包 404 | `sudo ./scripts/03-fix-uboot-tarball.sh`，然后重跑 01 |
| 只想先把 rootfs 拿出来试 | `sudo ./scripts/02-build-no-uboot.sh` |
| 想全部推倒重来 | `sudo ./scripts/00-setup.sh --recreate`（源码不动，只重建 chroot） |
| 第 2 步 `gcc` 是 12 之类的 | chroot 没建好，`--check` 看 `UBMC_CHROOT` 指到哪了 |

---

## 坑表汇总

### 构建

| 现象 | 原因 |
|---|---|
| `fdt_overlay.c` 报 libfdt 相关错误，或悄悄用了系统的旧头文件 | `buildroot/linux/linux.mk` 里有一行 `HOSTCFLAGS="$(HOSTCFLAGS)"` —— **拼错了**，Buildroot 用的是 `HOST_CFLAGS`。这行把内核自己的 `HOSTCFLAGS` 清空，丢了 `-Iscripts/dtc/libfdt`。**删掉那一行** |
| u-boot 源码包下载 404 | Buildroot 要的 `u-boot-2017.01+gitAUTOINC+590c7d7fe1` 是 Silicom 私有 commit，上游没有。见 [uboot.md](uboot.md) |
| `cc-option` 探测 `-march=armv7` 失败，退化到 armv5 | 新 gcc 不认旧写法。`arch/arm/Makefile` 写死 `-marmv7-a` |
| `include/compiler.h` 的 `##sfx` 报错 | 令牌拼接在新 gcc 下不合法，换成显式 `ULL` |
| libfdt 头文件冲突 | 系统的 `libfdt-dev` 和 u-boot 自带的 libfdt 打架。**卸载 `libfdt-dev`**，只留 `device-tree-compiler` |
| `configure` 报 `running as root` 拒绝 | 设 `export FORCE_UNSAFE_CONFIGURE=1`（构建脚本里已有） |

### 部署

| 现象 | 原因 |
|---|---|
| 串口只有一个 banner 然后什么都不打印 | 原厂 U-Boot 默认 `silent=1`，**U-Boot 活着**，不是砖。见 [chroot-and-recovery.md](chroot-and-recovery.md) |
| `WriteFile failed, win32 error 87` | 裸设备写入长度必须 **512 字节对齐**。`04-make-sdcard.ps1` 已自动补零 |
| `.NET FileStream` 写不了 `\\.\PhysicalDriveN` | 必须用 Win32 `CreateFile` / `WriteFile` P/Invoke。脚本里已实现 |
| 改 `boot0.scr` 之后机器像变砖 | hush 脚本 `source` 失败会掉到**静默**提示符。改前必须 `cp boot0.scr boot0.scr.orig` |
| `fatwrite` 之后文件变成 `-?????????` | 文件名带了开头斜杠。`dosfsck -a -v` 修 |

---

## 三条红线

1. **改 `boot0.scr` 前先备份成 `boot0.scr.orig`**（同一个 FAT 分区）。
   这是唯一能在静默 U-Boot 下盲发一条命令就还原的办法。
2. **不要碰主机侧的 SPI Flash / BlinkBoot**。那是主机的 SecureBoot 链条
   （`Locking SPI ... Already Lock the SPI` + 签名校验），改它必变砖，
   而且跟 BMC 完全没关系。
3. **先用 chroot 验证**，再考虑刷写。BMC 刷坏不影响主机当普通 x86 小主机用，
   但 BMC 自己的管理功能就没了。

---

## 参考

**跑通上面三步就不用读下面这几节。** 它们是查资料用的。

### 详细文档

| 文件 | 内容 |
|---|---|
| [buildroot.md](buildroot.md) | **主构建**：Debian stretch chroot + Buildroot，含那个卡了很久的 `HOSTCFLAGS` bug |
| [uboot.md](uboot.md) | U-Boot 单独构建：源码地址失效、11 个 vendor 补丁、3 个编译坑 |
| [sdcard.md](sdcard.md) | SD 卡安装盘布局与写卡脚本；以及**为什么 SD 启动走不通** |
| [chroot-and-recovery.md](chroot-and-recovery.md) | 在现跑 BMC 里 chroot 测试；串口救砖（`silent=1` 的真相） |

## 脚本

全部支持 `-h`。Linux 侧的都是 shell，跑之前先看 `scripts/env.sh` 里的路径。

| 脚本 | 跑在哪 | 作用 |
|---|---|---|
| `env.sh` | — | **路径配置**。改这一个文件，其余脚本都跟着变 |
| `chroot-lib.sh` | — | 挂载 chroot 的公用函数，被下面几个引用 |
| `00-setup.sh` | 宿主 Linux | **从零建 chroot**：debootstrap + 装依赖。`--check` 只体检，`--recreate` 重建 |
| `01-build.sh` | 宿主 Linux | 完整构建。`-j8` 并行，`uboot-custom` 只编一个包 |
| `02-build-no-uboot.sh` | 宿主 Linux | 跳过 u-boot 只出 rootfs（u-boot 编译失败时用） |
| `03-fix-uboot-tarball.sh` | 宿主 Linux | 重建上游已失效的 u-boot 源码包 |
| `ubmc-chroot.sh` | **BMC 上** | chroot 进一个 rootfs 目录测试，不重启、不动 eMMC |
| `serial-console.ps1` | Windows | 串口抓取 / 发送（`-ListPorts` 列端口、`-Seconds` 限时、`-Send` 发命令） |
| `04-make-sdcard.ps1` | Windows | 写 SD 安装盘（`-List` 先看盘号） |
| `05-sd-probe.cmd` | 卡上的 U-Boot | 打印 mmc 设备编号和分区表 |
| `press-setup-keys.ps1` | Windows | 串口上循环按 ESC/DEL/F2/F10 进固件设置 |
| `boot0-sd-first{,-v2}.{cmd,scr}` | BMC 的 U-Boot | 改过的 `boot0.scr`：先试 SD 再回退 eMMC，见 [uboot.md](uboot.md) |

---

## 整条链路

```
① 环境        源码仓库 + Debian stretch chroot（gcc 6、glibc 2.24）
                  ↓  scripts/00-setup.sh
② Buildroot   编译 uBMC（u-boot + kernel + rootfs）
                  ↓  scripts/01-build.sh
   产出        zImage · rootfs.cpio/ext2/tar · dt.dtb
                  ↓
③ U-Boot      （可选）单独编译，需要先重建失效的 tarball
                  ↓  scripts/03-fix-uboot-tarball.sh
④ 部署        A. chroot 进现跑 BMC 测试      ← 推荐，零风险
              B. 写 SD 安装盘（实测不可行，见 sdcard.md）
              C. 直接刷 eMMC（风险最高）
```

---

## BMC 的持久化分区

改 rootfs 不等于改持久配置。BMC 上真正跨重启保留的只有：

| 分区 | 挂载点 | 内容 |
|---|---|---|
| `mmcblk0p1` | — | FAT 启动分区（`MLO` / `u-boot.img` / `boot0.scr` / `zImage` …） |
| `mmcblk0p5` | `/config` | **mgmtd 配置库**（`is_mgmtd.conf` 等），口令/网络都在这里 |
| `mmcblk0p8` | `/var/images` | 4.5 G 可用，**放工具和测试 rootfs 的地方** |

`/etc`、`/tmp` 都在 initramfs/tmpfs 里，重启即丢。

---
---
