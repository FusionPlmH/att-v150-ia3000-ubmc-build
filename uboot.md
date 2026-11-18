# U-Boot 单独构建

[← 返回总览](README.md)

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

### SD 卡布局（`product/ubmc/scripts/upgrade/sdcard_env`）

```
MBR，单分区：FAT，起始 1 MiB，大小 1 GiB，boot 标志
根目录放：MLO / u-boot.img / boot.scr / dt.dtb / zImage / rootfs.cpio.uboot
```

再把 SD 版 `MLO` + `u-boot.img` 刷进 eMMC 的 FAT 分区（p1），
保留原厂 `boot0.scr`/`boot1.scr` 不动 —— 拔卡后刷回生产版 U-Boot 即可恢复。

完整操作步骤见 `../silicom-att-v150-sd/README-SD-BOOT.md`。

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

> `fatwrite` 的**文件名别带开头斜杠**（`/boot0.scr` 会在 FAT 根目录留下非法目录项，
> 要 `dosfsck -a -v /dev/mmcblk0p1` 修）。
