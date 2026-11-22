# 零风险验证 + 串口救砖

[← 返回总览](README.md)

既然 SD 启动走不通（见 [sdcard.md](sdcard.md)），
那"验证自己编的 rootfs"就用**在现跑 BMC 里 chroot**；
万一玩坏了，用**串口**救回来。

## 现在改东西的三条路（按风险从低到高）

| 想改什么 | 怎么做 | 代价 |
|---|---|---|
| 只改 **IPMI 守护进程** | 静态二进制丢进 `/config/firmware/`，`/etc/init.d/S47is_ipmi_lan restart` | 几秒，不重启、不刷机，见 [ipmi-lan.md](ipmi-lan.md) |
| 改 **整个 rootfs**（脚本、库、Web 栈） | chroot 里先验证（本文第一节）→ 满意了再刷 bank1（[firmware-upgrade.md](firmware-upgrade.md)） | 一次重启 |
| 玩坏了 | ① 断电重上电让 U-Boot 回滚 bank 0 ② 串口（本文第二节） | 几分钟 |

> ⚠️ 记住 `/` 是 **ramdisk**：往 `/usr`、`/etc` 里改的东西**重启就还原**。
> 要么写进 `/config`，要么打进镜像。这一条踩过不止一次。

---

## 一、chroot 测试法（推荐）

不重启、不动 eMMC 引导、不碰 SPI Flash。搞砸了删目录即可。

### 环境

| 项目 | 值 |
|---|---|
| chroot 目录 | `/var/images/testroot`（eMMC p8，4.5 G 可用，**持久**） |
| 体积 | 160 MB（完整拷贝 `/bin /sbin /usr /lib /etc /opt`） |
| 脚本 | [scripts/ubmc-chroot.sh](scripts/ubmc-chroot.sh) |

### 用法

```sh
/var/images/ubmc-chroot.sh                          # 交互 shell（进 /var/images/testroot）
/var/images/ubmc-chroot.sh -l                       # 列出 /var/images 下的候选 rootfs
/var/images/ubmc-chroot.sh /var/images/myroot       # 指定别的 rootfs
/var/images/ubmc-chroot.sh /var/images/myroot "cat /etc/product.txt; uname -m"
/var/images/ubmc-chroot.sh -u                       # 卸载伪文件系统
```

不是 rootfs 目录会直接拒绝，不会把宿主的 `/bin/sh` 当成 chroot 目标。
默认目录可以用 `UBMC_ROOTFS` 覆盖：`UBMC_ROOTFS=/var/images/x ubmc-chroot.sh`

脚本会先挂好伪文件系统再 `exec chroot`：

```sh
mount -t proc  proc  "$R/proc"
mount -t sysfs sysfs "$R/sys"
mount --bind /dev  "$R/dev"
mount --bind /run  "$R/run"
exec chroot "$R" /bin/sh
```

已验证 chroot 内 `/proc/cmdline`、`/proc/cpuinfo`、`/sys/class/mmc_host` 全部可读。

### rootfs 必须满足

| 要求 | 值 | 说明 |
|---|---|---|
| 架构 | **armv7l / armhf**（EABI hard-float） | 普通 armel、aarch64 都跑不了 |
| libc | **glibc ≤ 2.21** | BMC 自带 `ld-2.21.so`；更高版本会报 `GLIBC_2.xx not found` |
| init | 无 systemd | 没有真正的 PID 1，服务要手动起 |

### 把编译产物传上去

BMC 只有 249 MB `/tmp`（tmpfs）和 4.5 G 的 `/var/images`。
用 SSH 管道，别用 SD 卡中转（这个槽不可靠）：

```sh
# PC 侧（WSL）
tar -C my-rootfs -czf - . | ssh root@<bmc> "cat > /var/images/myroot.tar.gz"
ssh root@<bmc> "mkdir -p /var/images/myroot && tar -C /var/images/myroot -xzf /var/images/myroot.tar.gz"
```

### 四个注意点

1. **会跟正在跑的服务撞车**。要测自己的 `is_mgmtd` / `nginx` / `is_cli`，
   先停掉系统的：

   ```sh
   /etc/init.d/S45is_mgmtd stop      # ⚠️ 停了 BMC 网络会掉，先开好 SSH 或串口
   ```

   或者干脆让它们监听别的端口。

2. **`/proc`、`/sys`、`/dev` 是宿主的** —— `uname` 永远是 `4.14.54 armv7l`。
   **验证不了内核 / U-Boot / SPL 相关的东西**，只能验用户态。

3. **`/config` 挂在宿主上**，chroot 里看不到。要测就先
   `mount --bind /config $R/config`。

4. 结束后 `ubmc-chroot.sh -u` 卸载，`rm -rf` 删目录即可完全清除。

---

## 二、串口救砖：`silent=1` 的真相

> **先搞清一件事：前面板那个 RJ45 console 上其实有「两个」控制台。**
>
> 同一个串口（Windows 上枚举成 COM5 之类，`115200 8N1`）同时挂着
> ① 主机（xz 那头）的 console，② uBMC 自己的 Linux console。
> **在终端里按 `Ctrl+X` 可以在两者之间切换** —— 默认是主机控制台，
> 切过去才会看到 BMC 的登录提示。BMC 侧用 `root` + 它的 Linux 口令登录
> （出厂值见 [walkthrough 的「账号」一节](https://github.com/FusionPlmH/ia3000-ubmc-access-walkthrough#账号)）。
>
> 这一步是"失联之后最后的手段"：Web 挂了、SSH 挂了、IPMI 挂了，
> 只要串口在，Ctrl+X 进 BMC 控制台就能改配置、重启服务。
> 应急恢复具体做过一次（改 `ip link` 把 eth1 从 VRF 里摘掉导致 BMC 失联），
> 就是靠它救回来的。

一次失败的 `boot0.scr` 改动让机器"变砖"了 40 分钟，最后发现**根本没砖**。

### 症状

```
Initializing uBMC Firmware. Version UBMC_R102_20181205
（然后什么都没有，网络全断，BMC 的 IP 无 ARP 响应）
```

### 真相

原厂 U-Boot 的默认 env 里有 **`silent=1`**：

```c
// common/console.c: console_init_r()
s = env_get("silent");
if (s && *s == '1') gd->flags |= GD_FLG_SILENT;   // 之后所有 puts() 全部丢弃
```

`GD_FLG_SILENT` 在启动时置一次，**后来改 env 不会清除它**，`saveenv` 也不一定可用。

所以 **U-Boot 活着、在等输入、能执行命令，但一个字符都不打印** —— 看起来完全像死了。

> 为什么 SPL 的 banner 还能打印？因为 SPL 里没有这个 silent 逻辑。
> 内核输出也不受影响（由内核的 `loglevel` / `quiet` 控制）。

### 判断 U-Boot 是否还活着（一条命令）

往串口发 `reset`，看是否重新出现 SPL banner。
用 [scripts/serial-console.ps1](scripts/serial-console.ps1)：

```powershell
.\serial-console.ps1 -Send "reset\r" -Seconds 15
```

或者裸 PowerShell：

```powershell
$p = New-Object System.IO.Ports.SerialPort("COM3",115200,"None",8,"One")  # COM 号换成你的
$p.Open(); $p.Write("reset`r"); Start-Sleep 12; $p.ReadExisting()
```

| 结果 | 含义 |
|---|---|
| 出现 `Initializing uBMC Firmware...` | **U-Boot 活着**，可以救 |
| 完全没有 | 才是真砖（要动 SYSBOOT / JTAG） |

### 抢救：盲发启动命令

U-Boot 里 mmc 设备编号（来自 `board_get_boot_info() → silicom_autoboot_mmc(1, 0)`）：

| U-Boot 设备 | 硬件 |
|---|---|
| `mmc 0` | microSD（48060000） |
| `mmc 1` | **eMMC**（481d8000） |
| eMMC `:2` / `:3` | bank 0 / bank 1 的内核 + rootfs |

**用 `&&` 串联，加载失败就不会乱 `bootz`，避免把 CPU 跑飞：**

```text
setenv bootargs console=ttyS0,115200n8 root=/dev/ram0 rw loglevel=7; ext2load mmc 1:2 0x88000000 /dt.dtb && ext2load mmc 1:2 0x82000000 /zImage && ext2load mmc 1:2 0x88080000 /rootfs.cpio.uboot && bootz 0x82000000 0x88080000 0x88000000
```

**`loglevel=7` 去掉 `quiet` 是关键** —— 只有内核的输出不会被 `silent` 吃掉，
能直观确认加载成功。

启动后从 Linux 侧还原被改坏的文件：

```sh
mount /dev/mmcblk0p1 /mnt/fw
cp /mnt/fw/boot0.scr.orig /mnt/fw/boot0.scr     # 改之前先备份成 .orig
sync; umount /mnt/fw
```

### 或者盲发一条命令直接还原

```text
if fatload mmc 1:1 0x81000000 /boot0.scr.orig; then fatwrite mmc 1:1 0x81000000 boot0.scr 328; fi; reset
```

**前提是改之前做过 `cp boot0.scr boot0.scr.orig`。**

---

## 三、四条教训

1. **改 `boot0.scr` 前必须先备份**
   ```sh
   cp boot0.scr boot0.scr.orig      # 同一个 FAT 分区里
   ```
   这是唯一能在静默提示符下盲发一条命令就还原的办法。

2. **`boot0.scr` 语法照抄原厂风格**
   原厂是 5 条平铺命令，**没有任何 `if`**。
   复杂脚本（多行 `if`/`else`、引号里的 `[]`）hush 解析容易出问题，
   一旦 `source` 失败就掉到静默提示符，表现跟"变砖"一模一样。

3. **稳妥写法：只用 `&&` / `||` 链，最后一条永远是 eMMC 保底启动。**
   见 [scripts/boot0-sd-first-v2.cmd](scripts/boot0-sd-first-v2.cmd)。

4. **`/etc` 在 initramfs 里**（`root=/dev/ram0`），每次启动重建。
   只有 `/config`（mgmtd 的配置库）是持久的 —— 改 `/etc` 等于没改。

---

## 四、串口工具

这台机器没有显卡输出，所有操作都靠串口。
用 [scripts/serial-console.ps1](scripts/serial-console.ps1)（自动探测串口，不用指定 COM 号；
这台机上枚举成 **COM5**）：

```powershell
.\serial-console.ps1 -Seconds 30                  # 抓启动输出
.\serial-console.ps1 -Send "reset\r" -Seconds 20  # 给静默的 U-Boot 发 reset
.\serial-console.ps1 -StripAnsi -Seconds 60       # 去 ANSI，日志好读
```

> 想看的是 **uBMC 自己的控制台**（而不是主机）时，在这个串口终端里按一下 `Ctrl+X` 切换，
> 然后按 `Enter` 出登录提示。

另有 [scripts/press-setup-keys.ps1](scripts/press-setup-keys.ps1) ——
在串口上轮换按固件 Setup 键（进 BIOS/固件菜单用）。

---

## 五、刷 U-Boot 时怎么把砖风险降到 0（AM3352 SD 启动）

> 这一节针对「刷了 SD 版 U-Boot 之后起不来」。
> 完整版见工作目录 `ubmc-sd-boot/RECOVERY.md`。

### 5.1 关键结论：只刷 `u-boot.img`，**不要动 `MLO`**

`board/silicom/ubmc335x/board.c` 里：

```c
#ifdef CONFIG_BOARD_LATE_INIT
void board_late_init(void)          /* ← 第 509 行 */
{
    ...
    board_get_boot_info();          /* ← 第 562 行，SD/生产的分支在这里 */
    return 0;
}
#endif
```

整个函数被 `#if !defined(CONFIG_SPL_BUILD)` 包住 → **SPL 里不执行**。
也就是说 `mmcdev` 由 U-Boot proper 决定，原厂 SPL 照样能加载我们编的 `u-boot.img`。

> 出厂 `MLO` 里也能 `strings` 出 `Booting from bank`，只是因为
> `silicom_autoboot.c` 被 `#include` 进 `board.c`、SPL 也编译了这段 —— 但那是**死代码**。

**只换 `u-boot.img` ⇒ ROM 永远能找到合法 MLO ⇒ ROM 级的砖不会发生。**

### 5.2 「砖」分四级，只有最末一级要动硬件

| 级别 | 现象 | 谁还活着 | 怎么救 |
|---|---|---|---|
| **L1** | 串口有 `U-Boot ... =>` 提示符 | U-Boot 完整 | 直接敲命令 |
| **L2** | 串口静默，但发 `reset` 能看到 SPL banner | U-Boot（`silent=1`）| 盲发命令 |
| **L3** | 只有 `U-Boot SPL ...` 之后卡住 | SPL | 串口确认 + 重刷 u-boot.img |
| **L4** | 完全无输出 | 只有 ROM | SYSBOOT 硬件救援 |

> 我们编的 U-Boot 已把 `silent=1` 改成 `silent=0`，所以**刷了 SD 版 U-Boot 之后** L2 不会再出现。
> 但**现在机器上跑的仍是原厂 U-Boot**（我们只刷了 bank1 的 rootfs），所以 L2 依然适用。

### 5.3 刷前必做：回读校验

```sh
mount /dev/mmcblk0p1 /mnt/p1
cp /mnt/p1/u-boot.img /mnt/p1/u-boot.img.prod      # 备份
cp /tmp/u-boot.img /mnt/p1/u-boot.img.new
sync
sha256sum /tmp/u-boot.img /mnt/p1/u-boot.img.new   # 两个必须一致
mv /mnt/p1/u-boot.img     /mnt/p1/u-boot.img.orig
mv /mnt/p1/u-boot.img.new /mnt/p1/u-boot.img
sync; sha256sum /mnt/p1/u-boot.img; umount /mnt/p1
```

**回读校验过了再重启。** 这一步排除了「写坏」的全部可能。

### 5.4 L1 恢复命令（串口提示符下）

```
# 恢复原厂 u-boot.img
fatload mmc 1:1 0x81000000 /u-boot.img.prod
fatwrite mmc 1:1 0x81000000 u-boot.img $filesize
reset

# 或直接从 eMMC 正常启动（不插 SD）
ext2load mmc 1:2 0x88000000 /dt.dtb
ext2load mmc 1:2 0x82000000 /zImage
ext2load mmc 1:2 0x88080000 /rootfs.cpio.uboot
setenv bootargs console=ttyS0,115200n8 root=/dev/ram0 rw loglevel=7
bootz 0x82000000 0x88080000 0x88000000
```

设备号：**`mmc 0` = SD，`mmc 1` = eMMC**（U-Boot 与 Linux 相反）。
`fatwrite` 的文件名**不能带开头斜杠**，否则 FAT 根目录留非法目录项。

### 5.5 L4（ROM 级）硬件救援

只有换过 `MLO` 才可能到这里。

```
devmem 0x44E10040      # = 0x0040031C
                       # SYSBOOT[4:0] = 0b11100 → 只选 MMC1 (eMMC)
```

SYSBOOT 由板上**电阻分压**决定（不是拨码开关），改它要动烙铁：

| 路线 | SYSBOOT[4:0] | 做法 |
|---|---|---|
| UART 启动 | `0b00100` | ROM 从 UART0 用 XMODEM 收 SPL，TI `uart-boot` 发 MLO 到 RAM，再重写 eMMC |
| USB 启动 | `0b00011` | ROM 从 USB0 收 SPL，`usb-boot` 工具 |

最后手段是把 BGA 封装的 eMMC 拆下来用编程器写回全盘镜像。