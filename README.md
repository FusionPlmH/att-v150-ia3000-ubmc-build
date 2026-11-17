# uBMC 构建

把 Silicom **uBMC** 的固件从源码重新编译出来 —— 目标是把 BMC 刷成自己的版本，
或者只是先在现跑的 BMC 上验证功能再决定要不要刷。

设备：Silicom IA3000 / AT&T ATT-V150 的**管理卡**（BMC 部分）

| 项 | 值 |
|---|---|
| BMC 主控 | TI **AM3352BZCZ60**（ARM Cortex-A8 @600 MHz，armv7l / armhf） |
| BMC DDR3 | 512 MB（K4B4G16） |
| 启动介质 | Winbond SPI Flash（U47）+ 板载 eMMC |
| 源码 | <https://github.com/silicom-ltd/uBMC> |
| 出厂固件 | `UBMC_R102_20181205` |
| 串口 | `115200 8N1`，3.3V TTL，前面板 RJ45 console 口 |

> ⚠️ **构建是安全的，刷写才是危险的。**
> 想先验证自己的改动，用 [chroot-and-recovery.md](chroot-and-recovery.md) 里的 chroot 法，
> 零风险、不用重启、不动 eMMC 引导。

---

## 文档

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

## 产出物

Buildroot 正常跑完（`build exited rc=0`）后 `output/images/` 里是：

| 文件 | 大小 | 说明 |
|---|---|---|
| `zImage` | ~4.2 MB | ARM 内核 |
| `rootfs.cpio.uboot` | — | initramfs（正式启动用它，`root=/dev/ram0`） |
| `rootfs.ext2` | — | 同一个 rootfs 的磁盘镜像 |
| `rootfs.tar` | — | 打包形式 |
| `dt.dtb` | ~32 KB | 设备树 |

**注意 rootfs 是 cpio ramdisk**（`root=/dev/ram0`），不是分区上的文件系统。
所以**不能用 `ext4write` 直接改 `/etc/shadow`** —— `/etc` 每次启动都从 cpio 重建。

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

## 快速开始（照着敲就行）

**需要**：一台 Debian 12 / Ubuntu 22.04+ 的机器（物理机、VM、WSL 都行），
能用 root、能上网、至少 20 GB 空闲磁盘。内核版本无所谓 —— 编译在 chroot 里做。

```sh
# ① 取源码（约 1 GB）
git clone https://github.com/silicom-ltd/uBMC ~/uBMC

# ② 看一眼配置。默认路径都在 $HOME 下，一般不用改。
#    要改就编辑 scripts/env.sh，或者复制一份 env.local.sh 再改。
cd ubmc-build
./scripts/00-setup.sh --check

# ③ 建 chroot 并装依赖（debootstrap，首次约 5-10 分钟）
sudo ./scripts/00-setup.sh

# ④ 开始构建。Buildroot 是增量的，中断了重跑同一条命令就行。
sudo ./scripts/01-build.sh
```

跑完看 `$HOME/ubmc-work/build.log` 末尾的 `=== build exited rc=0 ===`，
以及 `~/uBMC/buildroot/output/images/` 里的产出物。

### 中途翻车了看这里

| 卡在哪 | 下一步 |
|---|---|
| `01-build.sh` 报 libfdt / `fdt_overlay.c` | [buildroot.md](buildroot.md) 的 `HOSTCFLAGS` 一节，删掉 `linux.mk` 里那一行再重跑 |
| `01-build.sh` 报 u-boot 源码包 404 | `sudo ./scripts/03-fix-uboot-tarball.sh`，然后重跑 01 |
| 只想先把 rootfs 拿出来试 | `sudo ./scripts/02-build-no-uboot.sh` |
| 想全部推倒重来 | `sudo ./scripts/00-setup.sh --recreate`（源码不动，只重建 chroot） |

### 换路径 / 换镜像源

```sh
# 临时改一次
UBMC_SRC=/data/uBMC UBMC_CHROOT=/data/stretch sudo -E ./scripts/00-setup.sh

# 或者长期改
cp scripts/env.sh scripts/env.local.sh
$EDITOR scripts/env.local.sh        # 改完所有脚本自动生效
```

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
