# SD 卡安装盘 —— 以及为什么 SD 启动走不通

[← 返回总览](README.md)

---

## 官方安装盘的布局

来自源码 `product/ubmc/scripts/upgrade/sdcard_env`：

```
0x00000000  清零前 1 MB
0x00020000（128 KiB）  MLO          ← ROM raw 模式偏移
0x00060000（384 KiB）  u-boot.img   ← 紧接 MLO 之后
0x00100000（1 MiB）     FAT16 分区，1 MiB → 1025 MiB
    根目录放 7 个文件：
      MLO  u-boot.img  boot.scr  zImage  dt.dtb  rootfs.cpio.uboot  <产品>.img
```

插卡上电 → 安装器问 `install?` → 答 `Y` → 自动写 eMMC 的 bank0 / bank1 → 重启 → 口令回到默认。

---

## 写卡脚本

[scripts/04-make-sdcard.ps1](scripts/04-make-sdcard.ps1)

```powershell
.\04-make-sdcard.ps1 -List                            # 列出可写磁盘（Card? 列有 YES）
.\04-make-sdcard.ps1 -DiskNumber 3                    # 写入，源文件取脚本所在目录
.\04-make-sdcard.ps1 -DiskNumber 3 -SourceDir D:\out  # 指定源目录
```

`-SourceDir` 里要放 `MLO` / `u-boot.img` / `boot.scr` / `dt.dtb` 四个文件，
全部是构建产物，仓库里没归档：

| 文件 | 从哪来 |
|---|---|
| `MLO` | Buildroot 输出 `output/images/MLO`（见 [buildroot.md](buildroot.md)） |
| `u-boot.img` | 同上 |
| `dt.dtb` | 同上，或 `output/build/linux-*/arch/arm/boot/dts/am335x-*.dtb` |
| `boot.scr` | 自己写的 `.cmd` 用 `mkimage` 编（见 [uboot.md](uboot.md)） |

### 它解决的两个坑

**① `.NET FileStream` 写不了 `\\.\PhysicalDriveN`**

`FileStream` 对裸设备会失败。必须走 Win32 API：

```csharp
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
public static extern IntPtr CreateFile(string name, uint access, uint share,
    IntPtr security, uint disposition, uint flags, IntPtr template);

[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool WriteFile(IntPtr h, byte[] buf, uint count,
    out uint written, IntPtr overlapped);
```

打开方式要注意：`access=0x40000000`（GENERIC_WRITE）、`share=3`（READ|WRITE）、
`disposition=3`（OPEN_EXISTING）。

**② 裸设备写入长度必须 512 字节对齐**

不补零就报：

```
WriteFile failed, win32 error 87
```

脚本里自动补：

```powershell
$pad = (512 - ($bytes.Length % 512)) % 512
```

### 写入流程

```
1. 写 0x20000 处的 MLO
2. 写 0x60000 处的 u-boot.img
3. 用 diskpart 建 1 MiB 起的 FAT32 分区并 format quick label=UBMCINST
4. 把 MLO / u-boot.img / boot.scr / dt.dtb 拷到根目录
```

> 两种约定都覆盖（raw 偏移 **和** FAT 里的文件），最大兼容 ——
> 不同版本的 SPL 可能从不同位置找 `u-boot.img`。

---

## ★ 结论：SD 启动**不可行**

做了三轮实验，结论明确。写下来省得以后再试。

### 一、ROM 层面：根本不读 SD

```
SYSBOOT[4:0] = 0b11100          （busybox devmem 0x44E10040 → 0x0040031C）
```

只选了 eMMC。实测：SD 卡上写好合法的 MLO（`0x20000`，GP header
`40 00 00 00 0c 00 00 00 ... CHSETTINGS`），重启后串口打印的**仍然是原厂 SPL**
`UBMC_R102_20181205` —— **SD 上的 MLO 从未被执行**。

**AM335x 的 ROM 只按 SYSBOOT 引脚 strap 决定的设备列表依次尝试**，
不是"有卡就读"。

### 二、U-Boot 层面：这个 SD 槽本身不可靠

厂商代码留了 SD 分支（`CONFIG_UMBC_SD_UBOOT` → `mmcdev=0`、`mmcbank=""` →
从 SD 读 `boot.scr`），理论上可以改 `boot0.scr` 让它先试 SD。**但实际做不通**：

- 把 `boot0.scr` 换成任何"先 `fatload mmc 0:1` 再回退 eMMC"的版本，机器都像变砖
- 原因不是语法（`&&` 链、单条平铺命令都试过），而是 **`fatload mmc 0:1` 碰到这个 SD 槽时会跑很长的 MMC 重试**
- 同一个槽在 Linux 里也要**手动 unbind/bind 驱动**才能识别卡：

  ```sh
  echo 48060000.mmc > /sys/bus/platform/drivers/omap_hsmmc/unbind; sleep 1
  echo 48060000.mmc > /sys/bus/platform/drivers/omap_hsmmc/bind
  ```

  `gpio-111 (|cd)` 的卡检测**不触发中断**。

**结论：这个槽硬件上有，但卡检测和 U-Boot 初始化都不可靠，不能作为启动路径。**

### 三、设备编号（U-Boot 与 Linux 相反）

| | U-Boot 里 | Linux 里 | 控制器 |
|---|---|---|---|
| microSD | `mmc 0` | `mmcblk1` | 48060000（AM335x MMC0） |
| eMMC | `mmc 1` | `mmcblk0` | 481d8000（AM335x MMC1） |

依据：`board/silicom → board_get_boot_info() { return silicom_autoboot_mmc(1, 0); }`
（eMMC = 1，SD = 0）。

> 这个反着的编号是很多人踩的坑 —— U-Boot 里 `mmc 1:2` 指的是 eMMC 的
> bank 0 内核分区，不是 SD。

自证脚本：[scripts/05-sd-probe.cmd](scripts/05-sd-probe.cmd)。用 `mkimage` 编成
`boot.scr` 放到卡上 FAT 分区根目录，U-Boot 起来后会 `mmc list` + 逐个
`mmc info / mmc part`，把两个控制器的真实编号和分区表打出来。

### 四、想 2 分钟自证

把卡上 `0x20000` 和 FAT 根目录的 `MLO` 都写成垃圾数据，再上电。
如果**仍然**正常启动并打出同样的横幅 → ROM 完全没读卡，SD 路线到此为止。

---

## 那还能怎么"在 SD 上跑自己的 uBMC"

只剩两条路：

| 路线 | 代价 |
|---|---|
| **改硬件 SYSBOOT 电阻**，让 ROM 从 SD（MMC0）启动 —— 位置疑似 AM3352 左侧 `R141/R138/R140/R139/R134` 这一排 | 动烙铁，风险高 |
| **不追求真启动**：在现跑的 BMC 里 chroot 到自己的 rootfs 做功能测试 | **零风险**，见 [chroot-and-recovery.md](chroot-and-recovery.md) |

实际选择的是第二条。对"验证我的改动对不对"这个目的，chroot 完全够用。
