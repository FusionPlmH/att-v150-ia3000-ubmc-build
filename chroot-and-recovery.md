# 零风险验证 + 串口救砖

[← 返回总览](README.md)

既然 SD 启动走不通（见 [sdcard.md](sdcard.md)），
那"验证自己编的 rootfs"就用**在现跑 BMC 里 chroot**；
万一玩坏了，用**串口盲发命令**救回来。

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
用 [scripts/serial-console.ps1](scripts/serial-console.ps1)（自动探测串口，不用指定 COM 号）：

```powershell
.\serial-console.ps1 -Seconds 30                  # 抓启动输出
.\serial-console.ps1 -Send "reset\r" -Seconds 20  # 给静默的 U-Boot 发 reset
.\serial-console.ps1 -StripAnsi -Seconds 60       # 去 ANSI，日志好读
```

另有 [scripts/press-setup-keys.ps1](scripts/press-setup-keys.ps1) ——
在串口上轮换按固件 Setup 键（进 BIOS/固件菜单用）。
