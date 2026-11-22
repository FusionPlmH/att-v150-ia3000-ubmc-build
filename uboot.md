# U-Boot 单独构建

[← 返回总览](README.md)

> **现状（第二十五轮）：机器上没有刷 U-Boot。**
> `/dev/mmcblk0p1`（FAT，128 MiB）里还是**出厂**的引导件，实测：
> `MLO` 60,228 B（sha256 `7fe8219e…`）、`u-boot.img` 435,668 B（sha256 `63ed4e20…`）、
> `boot0.scr` 与 `boot0.scr.orig` **哈希相同**（`26a4a41e…`，改过的版本已回退）、
> `uboot_version.txt` = `##UBMC_UBOOT_1.0.2_1d22f81a##`、`vendor.txt` = `##ATT##`。
> 二十多轮改动全部落在 **bank 1 的 rootfs**（`/dev/mmcblk0p3`）里，
> 所以本文关于"怎么编 U-Boot"的内容是**备用方案**，不是当前运行态。
> 也就是说：**`silent=1` 仍然在生效**，串口静默时按
> [chroot-and-recovery.md](chroot-and-recovery.md) 的 L2 处置。
>
> **已编出来的只有 SD 版**（`../ubmc-sd-boot/`，`CONFIG_UMBC_SD_UBOOT=y`：
> MLO 59,864 B / `fcac8c82…`、u-boot.img 432,452 B / `8ae7af48…`），
> 而且**从没上机跑过**。
>
> 真要的"生产版 + `silent=0`"**已经产出了**，但不是编出来的，而是给出厂镜像
> 打了一个**一字节补丁**（`63ed4e20…` → `71536094…`，只有 `0x39988` 不同），
> 见文末「不重编也能改：`silent` 一字节补丁」。它**尚未刷入** ——
> 刷引导是唯一没有远程回滚手段的操作，得等串口（COM5）在线。

---

## 为什么需要单独处理

Buildroot 会去下载一个特定名字的 U-Boot 源码包：

```
u-boot-2017.01+gitAUTOINC+590c7d7fe1-g590c7d7fe1.tar.gz
```

这个名字编码了一个 commit：**`590c7d7fe15d06a4d708403d3234bcd01cd039e1`**。

它是 Silicom/Meganet 的**私有 commit**，上游两个 u-boot 源都找不到。
Buildroot 尝试的镜像站（`vcp.meganet.ru`）也已经挂了 → 下载 404 → 整个构建卡住。

**解法：用上游 `v2017.01` + 厂商的 11 个补丁，等价重建那个 tarball，改成 Buildroot 期望的名字。**

---

## 那个 commit 是什么

```
commit 590c7d7fe15d06a4d708403d3234bcd01cd039e1
  "arm: am33xx: security: Fix size calculation on header"  2017-08-08
```

它是 **v2017.01 之后的一个上游补丁**（AM33xx 安全头长度计算修复），
被厂商打进了自己的分支。所以"v2017.01 + 补丁"就能还原出等价代码。

---

## 重建 tarball

脚本：[scripts/03-fix-uboot-tarball.sh](scripts/03-fix-uboot-tarball.sh)

### 它做的事

```
1. git clone --depth 1 --branch ti-u-boot-2017.01
   https://github.com/TexasInstruments/ti-u-boot
   （回退到 git.ti.com）

2. 依次打 product/ubmc/hardware/ti/uboot/patches/uboot-*.patch（11 个）

3. 改名为 Buildroot 期望的
   u-boot-2017.01+gitAUTOINC+590c7d7fe1-g590c7d7fe1

4. tar czf 到 buildroot/dl/   ← Buildroot 的下载缓存目录

5. rm -rf buildroot/output/build/uboot-custom   ← 清掉之前的失败状态
```

### ★ 基线必须是 TI 的分支，不是 vanilla u-boot

这是本项目最容易踩的坑。三个候选里只有第三个能用：

| 基线 | 结果 | 原因 |
|---|---|---|
| vanilla `v2017.01` | ✗ | 没有 `include/environment/ti/mmc.h`（v2017.05 才加）→ 编到一半 fatal error |
| vanilla `v2017.09` | ✗ | 结构体成员改名（`cm0ioctl`/`dt0ioctl` → 新版名字），`ddr.c` 补丁上下文对不上 |
| **TI `ti-u-boot-2017.01`** | **✓ 11/11 补丁干净应用** | 版本号写 `2017.01`、有 `mmc.h`、有 `dpll_ddr3_303MHz`/`HS_DEVICE`、**且保留 `cm0ioctl`** |

TI 的这个分支 = 2017.01 基础 + TI 自己后期补的板级增强，
正好是 Silicom/Meganet 打那 11 个补丁时用的基线。

验证方法：

```sh
cd src
grep -c 'define HS_DEVICE' arch/arm/include/asm/omap_common.h   # 应为 1
grep -c 'cm0ioctl'          arch/arm/include/asm/arch-am33xx/ddr_defs.h  # 应为 >0
ls include/environment/ti/                                      # 应有 mmc.h
sed -n '4,6p' Makefile                                          # VERSION=2017 PATCHLEVEL=01
```

打完补丁后**不应该有 `.rej` 文件**：

```sh
find . -name '*.rej'    # 必须为空
```

### 输出长这样

```
=== 1. clone u-boot v2017.01 ===
  当前: v2017.01  <sha>
=== 2. 依次打 11 个 vendor 补丁 ===
  uboot-0001-....patch                       OK
  uboot-0002-....patch                       OK
  ...
=== 3. 打包成 Buildroot 期望的名字 ===
=== 4. 清掉旧构建目录 ===
```

补丁已经是"已应用过"状态会显示 `已应用过`，不算失败。
只要最后没有 `!! 失败:` 那一行就行。

### 打包的关键细节

```sh
tar cjf "$DL/$NAME.tar.gz" --exclude=.git "$NAME"
```

- **必须 `--exclude=.git`** —— Buildroot 期望的是发布用的 tarball，带 `.git` 会让
  解包校验或后续的 `git describe` 行为不一致
- **顶级目录名必须和 tarball 名一致**（去掉 `.tar.gz`）。Buildroot 解包后进的是
  `$NAME/`，名字对不上就找不到

---

## 三个编译坑（用 vanilla 基线时才会遇到）

> ★ 用 **TI `ti-u-boot-2017.01` 分支**（见上面「基线必须是 TI 的分支」）时，
> 这三个坑都**不会出现** —— 因为厂商基线本来就带着这些修复。
> 只有退回去用 vanilla u-boot 才需要手工处理。

编 `ubmc335x_install_defconfig`（UBMC-SD-INSTALL）时踩到的，全部已修。

### ① `-march=armv7` 探测失败，退化到 armv5

`arch/arm/Makefile` 里用 `cc-option` 探测 `-march=armv7`。
新 gcc 不认这个写法（它要 `-march=armv7-a`），探测失败 → 静默退化到 **armv5**
→ 编出来的东西跑不了（AM3352 是 Cortex-A8，armv7）。

**修法：写死 `-march=armv7-a`。**

```make
# arch/arm/Makefile
-march=armv7-a
```

> 这个坑很隐蔽：编译**不报错**，只是产物在板子上跑不起来或者性能诡异。
> 怀疑时用 `readelf -A` 看产物里标的 arch 对不对。

### ② `##sfx` 令牌拼接不合法

`include/compiler.h` 里有一处用 `##sfx` 拼接令牌，新 gcc 报错。

**修法：换成显式 `ULL` 后缀。**

```c
/* 原来是 ULL 的令牌拼接，改成直接写 */
#define _ULL(x) x##ULL
```

### ③ `libfdt-dev` 和 u-boot 自带的 libfdt 打架

系统装的 `libfdt-dev` 提供 `/usr/include/libfdt.h`，u-boot 自带一份自己的。
两者版本不同，谁先被找到就编出不一样的东西。

**修法：卸掉系统的。**

```sh
apt-get remove -y libfdt-dev
apt-get install -y device-tree-compiler    # dtc 还是要的
```

> 这跟 [buildroot.md](buildroot.md) 里那个 `HOSTCFLAGS` bug 是**同类问题的两个面**：
> 都是"头文件从错误的地方被找到"。一个是因为 `-I` 丢了，一个是因为系统里也有同名头文件。

---

## 产出物

| 文件 | 大小 | 说明 |
|---|---|---|
| `MLO` | 49 KB | TI GP 头：`40 00 00 00 0C 00 00 00` |
| `u-boot.img` | 417 KB | FIT 镜像 |
| `dt.dtb` | 32 KB | 设备树 |
| `boot.scr` | 538 B | 编译后的启动脚本 |

`MLO` 是 **SPL**（第一阶段），由 ROM 直接加载；`u-boot.img` 是完整的 U-Boot。

---

## 编 SD / install 版 U-Boot（`CONFIG_UMBC_SD_UBOOT`）

Silicom 官方支持从 SD 卡启动，靠的是 U-Boot 的一个编译开关。
这是**测试自编译固件最安全的路径**：eMMC 里的原厂固件完全不动，拔卡即回。

### 两套 defconfig 的区别

| defconfig | `CONFIG_UMBC_SD_UBOOT` | 行为 |
|---|---|---|
| `ubmc335x_defconfig` | 未设（生产版） | 探测 eMMC 号 → `mmcdev=<emmc>`，`mmcbank=0/1`，读 `boot0.scr`/`boot1.scr` |
| **`ubmc335x_install_defconfig`** | **`=y`** | `mmcdev=0`（SD），`mmcbank=""`，读 `boot.scr` |

源码 `uboot-0003-boot_fix.patch`：

```c
#if defined(CONFIG_UMBC_SD_UBOOT)
	setenv("mmcdev", "0");
	setenv("mmcbank", "");
#else
	... 探测 eMMC、读 bank 标志位 ...
#endif
	setenv("bootcmd", "run loadbootscript; run bootscript;");
```

配套脚本 `product/ubmc/hardware/ti/uboot/scripts/boot-sd.txt`：

```
fatload mmc 0:1 0x88000000 /dt.dtb
fatload mmc 0:1 0x82000000 /zImage
fatload mmc 0:1 0x88080000 /rootfs.cpio.uboot
setenv bootargs console=ttyS0,115200n8 root=/dev/ram0 rw
bootz 0x82000000 0x88080000 0x88000000
```

### 构建步骤（实测通过）

```sh
cd <TI 源码树>
cp product/ubmc/hardware/ti/uboot/configs/ubmc335x_install_defconfig configs/

export PATH=<buildroot>/output/host/bin:$PATH
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- ubmc335x_install_defconfig

# 跳过 pylibfdt 的 python 绑定（buildroot 的 host python2.7 缺 libpython2.7.so）
mkdir -p tools && touch tools/_libfdt.so tools/libfdt.py

make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j16
```

确认配置：

```
CONFIG_ARM=y
CONFIG_CPU_V7=y
CONFIG_TARGET_UBMC335X=y
CONFIG_SYS_BOARD="ubmc335x"
CONFIG_UMBC_SD_UBOOT=y          ← 关键
```

### 验证产物真的是 SD 版

生产分支的字符串在 SD 版里必须**消失**：

```sh
strings -a MLO u-boot.img | grep -E 'Failed to get emmc device|Booting from bank'
# 无输出 = SD 版正确
```

出厂固件（生产版）里这两条都在。这条判据比 `build_sd_install.sh` 里那个
`grep bootcmd | grep fatload` 更可靠 —— 因为 SD 版的 `bootcmd` 其实是
`run loadbootscript; run bootscript;`，本身不含 `fatload`。

架构确认：

```sh
arm-linux-gnueabihf-readelf -A spl/u-boot-spl | grep Tag_CPU
#   Tag_CPU_name: "7-A"
#   Tag_CPU_arch: v7
```

### 实测产物对照

| | 出厂（生产版） | 我们（SD 版） |
|---|---|---|
| `MLO` | 60,228 | 59,864 |
| `u-boot.img` | 435,668 | 432,452 |
| `dt.dtb` | `23c76bca…` | **`23c76bca…` 完全一致** |

`dt.dtb` 哈希与出厂逐字节相同，说明设备树构建是忠实的。

### ★ 为什么「只刷 `u-boot.img`」就足够切到 SD 启动

`CONFIG_UMBC_SD_UBOOT` 影响两处，但只有一处在 SPL 里：

| 位置 | 作用 | 编译进 |
|---|---|---|
| `silicom_autoboot.c:19` | `mmcdev=0`、`mmcbank=""` | **只在 U-Boot proper**（被 `board_late_init()` 调用，而该函数整体在 `#if !defined(CONFIG_SPL_BUILD)` 内）|
| `board.c:730` | `board_mmc_init()` 初始化 SD 或 eMMC | **SPL 和 proper 都编译**，但只有 SPL 真正调用 |

关键在本板 **`CONFIG_SPL_OF_CONTROL` 未开**：

| 阶段 | MMC 枚举方式 | `board_mmc_init()` 是否起作用 |
|---|---|---|
| SPL (`MLO`) | 非 DM，`cur_dev_num++` 只注册一个控制器 | ✅ 起作用 → 原厂 MLO 只认 eMMC |
| U-Boot proper | `CONFIG_DM_MMC=y`，驱动模型遍历 DT：`&mmc1`(SD)=0、`&mmc2`(eMMC)=1 | ❌ 死代码 |

所以只要**保留原厂 `MLO`**（它保证从 eMMC p1 加载 `u-boot.img`），
把 SD 版 `u-boot.img` 写进 eMMC p1 即可：

```
ROM → eMMC p1 MLO(原厂) → 从 eMMC p1 加载 u-boot.img(我们)
      → mmcdev=0 → boot.scr 从 SD p1 读 → 内核/根文件系统全从 SD 读
```

U-Boot proper 里两张卡始终都在（`mmc 0`=SD、`mmc 1`=eMMC），
所以出问题时 `fatwrite mmc 1:1 …` 依然能回写 eMMC。
**代价为零，且 ROM 级砖（L4）不会发生。**

反之，如果把 SD 版 `MLO` 也刷进 eMMC，SPL 就只初始化 SD ——
一旦 SD 卡不在，连 U-Boot 都起不来。**不要那样做。**

> `load mmc ${mmcdev} …` 没写分区号时默认取 **1**（`disk/part.c`：
> `part == PART_UNSPECIFIED` → `part = 1`），所以 `load mmc 0` 读的是 SD 的 p1，不是整盘。

### SD 卡布局（`product/ubmc/scripts/upgrade/sdcard_env`）

```
MBR，单分区：FAT，起始 1 MiB，大小 1 GiB，boot 标志
根目录放：MLO / u-boot.img / boot.scr / dt.dtb / zImage / rootfs.cpio.uboot
```

`boot.scr` 全部用 `fatload mmc 0:1`，所以 6 个文件都放**同一个 FAT 分区**即可，
不需要 ext4 分区。

只把 SD 版 `u-boot.img` 刷进 eMMC 的 FAT 分区（p1），
原厂 `MLO` / `boot0.scr` / `boot1.scr` 全部保留 —— 拔卡或刷回即恢复。

完整操作步骤见工作目录的 `ubmc-sd-boot/README-SD-BOOT.md`（本文档同源）。

---

## 编译 U-Boot 启动脚本（`.cmd` → `.scr`）

`boot0.scr` 是 `mkimage` 包装过的 U-Boot 脚本。源文件是 `.cmd`：

```sh
mkimage -A arm -O linux -T script -C none -n "UBMC boot0 SD-first" \
        -d boot0-sd-first.cmd boot0-sd-first.scr
```

`.scr` 头部就是那段二进制（`'V…UBMC boot0 SD-first`），
后面跟着明文脚本正文 —— 用 `cat` 也能看出内容。

### 两个脚本版本（都在 `scripts/`）

**`boot0-sd-first.cmd`** —— 带分支判断的版本：

```
setenv silc_sd 1
如果三个文件都能从 SD 卡 fatload → 从 SD 启动
否则 → 回退 eMMC bank 0
```

**`boot0-sd-first-v2.cmd`** —— 简化成 `&&` 链：

```
fatload ... && fatload ... && fatload ... && bootz ...
（失败就顺序往下执行 eMMC 分支）
```

`scripts/` 里同时放了编好的成品 `boot0-sd-first.scr` / `boot0-sd-first-v2.scr`，
拷进 FAT 分区改个名就能用，不用自己装 mkimage：

```sh
# 在 BMC 上（或 chroot 里）
cp /mnt/p1/boot0.scr /mnt/p1/boot0.scr.orig          # 先备份！
cp scripts/boot0-sd-first-v2.scr /mnt/p1/boot0.scr
```

> **v2 更安全。** 原厂 `boot0.scr` 是 5 条平铺命令、没有任何 `if`。
> 复杂 hush 脚本（多行 `if`/`else`、引号里的 `[]`）一旦 `source` 失败，
> 就掉到**静默**提示符 —— 表现跟变砖一模一样。
>
> **稳妥写法：只用 `&&` / `||` 链，最后一条永远是 eMMC 保底启动。**

---

## 警告：改 `boot0.scr` 之前

**先在同一个 FAT 分区里备份：**

```sh
cp boot0.scr boot0.scr.orig
```

这是唯一能在静默 U-Boot 下**盲发一条命令**就还原的办法：

```text
if fatload mmc 1:1 0x81000000 /boot0.scr.orig; then fatwrite mmc 1:1 0x81000000 boot0.scr 328; fi; reset
```

（实测有效：改坏 `boot0.scr` 那次的"变砖"就是这么一条命令救回来的。
现在 p1 上 `boot0.scr` 与 `boot0.scr.orig` 已经是同一个哈希 `26a4a41e…`。）

---

## 不重编也能改：`silent` 一字节补丁（**推荐路径**）

出厂 `u-boot.img` 不是 legacy uImage，而是 **FIT 镜像**（一个 DTB，magic `0xd00dfeed`）：

```
FIT totalsize  897 B（数据区从 0x384 开始）
/images/firmware@1   data-offset=0        size=401752   ← 真正的 U-Boot 二进制
/images/fdt@1        data-offset=401752   size=33014    ← 设备树（与出厂 sha256 相同）
version  U-Boot 2017.01UBMC_R102_20181205-00360-gc6c77f9f4f-dirty for ubmc335x board
hash 节点  NONE          ← 所以改数据**不需要**重算任何校验和
```

二进制未压缩，默认环境串里的 `silent=1` 就原样躺在里面（文件偏移 `0x39981`，
在 payload 内 `+0x395FD`）。**把那个 `1` 改成 `0`** 就得到「生产版 + 串口有输出」，
其余一个字节都不动：

```sh
# 打印现状（不改文件）
python scripts/patch-uboot-silent.py check u-boot.img

# 产出生产版 + silent=0
python scripts/patch-uboot-silent.py patch  u-boot.img  u-boot-silent0.img

# 想退回 silent=1
python scripts/patch-uboot-silent.py patch --value 1 u-boot.img u-boot-silent1.img
```

实测（出厂 `63ed4e20…` → 补丁版 `71536094…`，都是 435,668 B）：

| | 出厂 | 补丁版 |
|---|---|---|
| 字节差异 | — | **1 个**（`0x39988`：`'1'`→`'0'`）|
| `silent=1` / `silent=0` | 1 / 0 | 0 / 1 |
| `Booting from bank`（生产版特征）| 3 | **3** |
| `Failed to get emmc device` | 1 | **1** |
| FIT 结构 / 版本串 / 大小 | — | **完全不变** |

对照组：我们自己编的 SD 版 `u-boot.img`（`8ae7af48…`）里 `Booting from bank` 与
`Failed to get emmc device` **都是 0** —— 这正是它不能当生产版用的字符串级证据。

> 工具会拒绝三种情况：不是 FIT、payload 里有 hash 节点（那才需要重算）、
> payload 里 `silent=` 出现次数不为 1。补完自己再 `check` 一遍即可。

### 刷写：`scripts/flash-uboot.sh`

在 BMC 上跑：

```sh
sh scripts/flash-uboot.sh -n /var/images/u-boot.img   # 先 dry-run，只报告
sh scripts/flash-uboot.sh    /var/images/u-boot.img   # 备份 + 写入 + 回读校验
reboot
```

脚本的行为（每一步都挡住一种翻车方式）：

1. 新的 `u-boot.img` 开头不是 `d00dfeed` 直接拒绝；
2. 把当前 p1 的 `u-boot.img` 复制到 `/var/images/uboot-backup/<时间戳>`，
   并在 p1 上留一份 `u-boot.img.prod`（**只在不存在时创建**，不会被覆盖）；
3. 先写 `u-boot.img.new` → `sync` → 回读 `sha256` 与源一致 → **才** `mv` 覆盖；
4. 全程不碰 `MLO`、`boot0.scr`、`boot1.scr`，也不碰 `0x10000`/`0x11000` 两个标志。

因为 `boot flag` 不变，重启后仍然进同一个 bank，只是这次串口会说话。

回滚有一条"串口静默也能用"的盲发命令（前提是第 2 步留了 `.prod`）：

```text
fatload mmc 1:1 0x81000000 /u-boot.img.prod; fatwrite mmc 1:1 0x81000000 u-boot.img $filesize; reset
```

---

## 还差什么（U-Boot 侧的真实缺口）

| 想要的 | 现状 |
|---|---|
| SD / install 版 U-Boot | ✅ 已编出（`../ubmc-sd-boot/`），产物哈希有记录，**但从没上机跑过** |
| **生产版 + `silent=0`** | ✅ **已用一字节补丁产出**（`71536094…`，435,668 B，见上一节），**尚未刷入** |
| SD 启动能用 | ❌ 判死（ROM 不读 SD / 要动 SYSBOOT 电阻），见 [sdcard.md](sdcard.md) |
| `boot0.scr` 层面的定制 | 已试过（`boot0-sd-first{,-v2}.scr`），最终回退到出厂脚本 |
| 从源码重编生产版 | 可选，非必需 —— 一字节补丁的行为等价；只有在想改 `bootcmd`/加命令时才需要 |

**为什么不能直接刷现成的 SD 版**：它带 `CONFIG_UMBC_SD_UBOOT=y`，会把
`mmcdev` 设成 `0`（SD 卡）并去读 `boot.scr`。没有可用的 SD 启动，刷它等于
把 eMMC 里的系统"看不见了"。字符串级证据：SD 版里 `Booting from bank` 与
`Failed to get emmc device` 都是 0，生产版各有 3 和 1。

刷写三条铁律（与上一节的脚本一致）：

1. **只刷 `u-boot.img`，绝不动 `MLO`** —— 原厂 SPL 保证 ROM 永远能找到合法 MLO，
   所以 ROM 级（L4）砖不会发生；
2. 刷前 `sha256sum` 回读校验，刷后再校验一次；
3. **串口必须在线**（这台机是 COM5）—— 没有串口就看不出 `silent` 到底改成功没有，
   也没法在起不来时盲救。

> 从源码重编的完整步骤见本文档上半部分（`ubmc335x_defconfig` + 把
> `include/configs/ubmc335x.h` 的 `silent` 从 1 改成 0）。它和一字节补丁的区别
> 只在于"顺带把整个 U-Boot 换成自编译版"—— 对当前目标而言是多余的。

### 备选方案：不刷引导，只改 saved env

如果 `silent=1` 落在 **U-Boot 保存的 env** 里（不是只有编译进去的默认值），
理论上可以用 `fw_setenv silent 0` 改掉，一行不动引导。

但这条路有两个前提，现在都**没有**验证过：

- 得先 dump 出当前的 env 并**原样回写**（只加 `silent=0`）。U-Boot 读到合法 env
  时**不会**再把编译默认值合并进来，写残了会丢 `bootcmd` → 变成真"静默+不启动"；
- 需要串口兜底。

设备上现在的状况：`/proc/mtd` 里没有分区、也没有 `fw_printenv`/`fw_setenv`，
真要走这条得先摸清 SPI NOR 的 env 偏移和冗余两份的布局。
**在串口接回来之前，这条路不要动。**

> `fatwrite` 的**文件名别带开头斜杠**（`/boot0.scr` 会在 FAT 根目录留下非法目录项，
> 要 `dosfsck -a -v /dev/mmcblk0p1` 修）。
