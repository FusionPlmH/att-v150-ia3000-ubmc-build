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
1. git clone --depth 1 --branch v2017.01 u-boot
   源顺序：git.u-boot-project.org → github.com/u-boot/u-boot（自动回退）

2. 依次打 product/ubmc/hardware/ti/uboot/patches/uboot-*.patch（11 个）

3. 改名为 Buildroot 期望的
   u-boot-2017.01+gitAUTOINC+590c7d7fe1-g590c7d7fe1

4. tar cjf 到 buildroot/dl/   ← Buildroot 的下载缓存目录

5. rm -rf buildroot/output/build/uboot-custom   ← 清掉之前的失败状态
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

## 三个编译坑

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
