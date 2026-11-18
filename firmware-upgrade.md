# uBMC 的 A/B 固件升级机制

**核心结论：升级固件和升级 U-Boot 是两件正交的事。**

升级 BMC 固件只是往分区里写**文件** + 改**两个字节的标志**，
`MLO` / `u-boot.img` / `boot0.scr` / `boot1.scr` 全都不用动。

---

## 槽位与分区

| 槽 | 分区 | 文件系统标签 | 内容 |
|---|---|---|---|
| bank 0 | p2 | `SYSTEM1` | `zImage`、`rootfs.cpio.uboot`、`dt.dtb`、`software_version.txt` |
| bank 1 | p3 | `SYSTEM2` | 同上 |

p1（`FIRMWARE`，FAT16）放 `MLO`、`u-boot.img`、`boot0.scr`、`boot1.scr`、
`uboot_version.txt`、`vendor.txt`。

启动脚本就这么分工（`boot0.scr` / `boot1.scr` 内容）：

```
boot0.scr: ext2load mmc ${mmcdev}:2 0x88000000 /dt.dtb      ← bank 0 → p2
boot1.scr: ext2load mmc ${mmcdev}:3 0x88000000 /dt.dtb      ← bank 1 → p3
```

路径定义全在 `/etc/product/UBMC/upgrade/upgrade_var` 里：

```sh
uimage_bank0_addr = SYSTEM1/zImage
rootfs_bank0_addr = SYSTEM1/rootfs.cpio.uboot
uimage_bank1_addr = SYSTEM2/zImage
rootfs_bank1_addr = SYSTEM2/rootfs.cpio.uboot
software_boot_flag_addr    = MMCDEV/0x10000
software_upgrade_flag_addr = MMCDEV/0x11000
```

全是**按分区标签的文件路径**，不是裸 dd。

## 两个标志字节

| 名称 | 偏移 | 位置说明 | 出厂实测 |
|---|---|---|---|
| boot flag | `0x10000`（65536） | p1 之前的空隙区 | `'0'`（0x30）|
| upgrade flag | `0x11000`（69632） | 同上 | `'0'`（0x30）|

`0x10000`/`0x11000` 落在 MBR 与 p1（LBA 2048 = 1048576）之间的空闲区，
所以读写标志不会碰到任何分区。

含义（`is_upgrade.sh` 里的原始注释）：

```
# boot flag(0|1) select which image bank will be used.
# upgrade flag, 0: normal, without upgrade
#               1: upgrade finished, need check (uboot will change it to 2)
#               2: start check (after boot up, app will change it to 0;
#                  uboot think upgrade is failed, will change to 0, and downgrade)
```

## 状态机

```
① 固件/我们：写新文件到 bank 1
② 固件/我们：boot flag 翻到 1，upgrade flag = 1
③ 重启 → U-Boot 读到 1 → 改成 2，从 bank 1 启动
④ 新固件起来 → 确认 → upgrade flag 改回 0        ← 升级「落定」
⑤ 若 ④ 没发生就复位 → U-Boot 下次读到 2
                      → 翻回 bank 0，并把 flag 清 0   ← 自动回滚
```

对应 U-Boot 源码（`board/silicom/silicom_autoboot.c`）：

```c
if (flags[UPGRADE] == '2') {          /* 上次升级没确认 → 回滚 */
        flags[UPGRADE] = '0';
        flags[BOOT] = (flags[BOOT] == '1') ? '0' : '1';
} else if (flags[UPGRADE] == '1') {   /* 升级后第一次启动 */
        flags[UPGRADE] = '2';
}
```

## ★ 第 ④ 步「谁确认」——一个容易踩的坑

`is_upgrade.sh -C`（`confirm_software_upgrade()`）把 `upgrade flag` 从 `2` 改成 `0`。

但**出厂 rootfs 里没有任何 shell 脚本或 Lua 调它** ——
`grep -rn 'is_upgrade.sh.*-C' /` 只会命中 `is_upgrade.sh` 自己的帮助文本和参数解析。

真正的调用方藏在管理守护进程里：

```
/usr/bin/is_mgmtd   →  字符串  "/usr/sbin/is_upgrade.sh -C"
                            and  "/action/system/check-upgrade"
```

即 `is_mgmtd` 处理 `/action/system/check-upgrade` 这个 action 时会跑 `is_upgrade.sh -C`，
而 **Web 的升级页 `/admin/system/upgrade` 打开时就会触发这个 action**。

### 由此得到两条实用结论

1. **升级后要确认**：新固件启动后打开一次 Web 升级页，或手动执行
   `is_upgrade.sh -t fw-upgrade -v 0`。
2. **不要用 `is_upgrade.sh -c` 收尾** —— `clear_flags()` 是这么写的：

   ```sh
   clear_flags()
   {
       is_upgrade_set_flag $software_boot_flag_addr "0"      # ← 会切回 bank 0
       is_upgrade_set_flag $software_upgrade_flag_addr "0"
   }
   ```

   只清 upgrade flag 才是 `-t fw-upgrade -v 0`。

## 升级包是 GPG 签名的

```sh
gpg --decrypt ${package_file} 1> $tmp_tar 2>/dev/null \
    || error_quit "Invalid upgrade package: Corrupted content"
```

公钥在 `/etc/silicom-is-public.key`，私钥在 Silicon 手里。
**自编译的包无法走 Web 上传** —— 但拿到 root 后可以手动做等价的事。

## Web 上的 "Switch" 按钮是什么

`/usr/sbin/is_upgrade.sh -w` → `switch_software()`：

```sh
upgrade_flag=$(is_upgrade_get_flag $software_upgrade_flag_addr)
if [ ! $upgrade_flag -eq "0" ]; then
        error_quit "Please reboot the system to fininsh last upgrade" "0"
fi
new_boot_flag="0"
boot_flag=$(is_upgrade_get_flag $software_boot_flag_addr)
elif [ "$boot_flag" == "0" ]; then new_boot_flag="1"; fi
is_upgrade_set_flag $software_boot_flag_addr $new_boot_flag
is_upgrade_set_flag $software_upgrade_flag_addr "1"
```

只翻标志、不动任何固件 —— 纯粹的槽位切换，零风险。
（前提是 upgrade flag 必须是 0，否则拒绝执行。）

## 手动把自编固件装进 bank 1

前提：`/root/uBMC-fork/buildroot/output/images/` 里已有自己的 `zImage` 和 `rootfs.cpio.uboot`。

```sh
# ① 挂 bank1，先把原固件留底
mkdir -p /mnt/p3 && mount /dev/mmcblk0p3 /mnt/p3
mkdir -p /tmp/bank1-backup
cp -a /mnt/p3/zImage /mnt/p3/rootfs.cpio.uboot \
      /mnt/p3/dt.dtb /mnt/p3/software_version.txt /tmp/bank1-backup/

# ② 先写临时名
cp /tmp/up/zImage             /mnt/p3/zImage.new
cp /tmp/up/rootfs.cpio.uboot  /mnt/p3/rootfs.cpio.uboot.new
sync
sha256sum /mnt/p3/zImage.new /mnt/p3/rootfs.cpio.uboot.new   # 必须与源一致

# ③ 校验通过才替换
mv /mnt/p3/zImage.new            /mnt/p3/zImage
mv /mnt/p3/rootfs.cpio.uboot.new /mnt/p3/rootfs.cpio.uboot
printf '##UBMC_1.2.0.8_BUILD_local##\n' > /mnt/p3/software_version.txt
sync; umount /mnt/p3

# ④ 置标志（官方 debug 通道，比 dd 干净）
is_upgrade.sh -t fw-boot    -v 1
is_upgrade.sh -t fw-upgrade -v 1
is_upgrade.sh -s            # 期望 boot:1 upgrade:1
```

然后重启。串口会看到 `Booting from bank 1`。

**版本号格式要求**：`software_version.txt` 必须以 `##` 开头结尾
（`is_upgrade.sh` 注释：`version must start with ## and end with ##`）。
出厂值形如 `##UBMC_1.2.0.7_BUILD_202e3c40##`。

新固件起来后确认落定：

```sh
is_upgrade.sh -s                       # upgrade flag 应为 0
# 若还是 2（说明没触发 check-upgrade），手动清：
is_upgrade.sh -t fw-upgrade -v 0
```

## 安全边界

- **bank 0（p2）全程不动** —— 那里是当前正常工作的固件，才是真正的安全网
- 新固件起不来 → 手动复位 → U-Boot 自动回滚到 bank 0
- 只有 bank 1 上原来的旧版被覆盖，所以动手前先 `cp -a` 留底
- 这条路**完全不碰 U-Boot**

### 回滚的前提与陷阱

回滚是**纯标志驱动**的，没有任何健康检查。所以：

| 情况 | 结果 |
|---|---|
| 新固件挂死 → 断电重上电 | ✅ 回滚 bank 0 |
| 新固件能起，但没清 upgrade flag，之后正常重启 | ⚠️ **也回滚** |
| 新固件能起，且清了 upgrade flag | 停在 bank 1 |

想留在 bank 1，起来后**必须**手动清：

```sh
is_upgrade.sh -t fw-upgrade -v 0    # 只清 upgrade，保持 boot=1
```

**不要用 `is_upgrade.sh -c`**（见上文，`clear_flags()` 会把 boot 也清成 0）。

「硬 reset」= 整个 V150 断电重上电 —— uBMC 是独立 SoC，待机供电，
按前面板 reset 未必能把它复位，拔电最保险。

> 原厂 U-Boot 的默认 env 里是 `silent=1`（`include/configs/ubmc335x.h:152`），
> 所以串口看不到 `Booting from bank 1` 这类 U-Boot 打印；
> 但 `boot1.scr` 里的 `console=ttyS0,115200n8` 会让内核日志正常输出，
> 足以判断走到了哪一步。

## 实测数据（ATT V150 出厂机）

```
boot flag  = '0'    upgrade flag = '0'
bank 0 (p2)  UBMC_1.2.0.8_BUILD_1d22f81a
bank 1 (p3)  UBMC_1.2.0.7_BUILD_202e3c40
uboot        UBMC_UBOOT_1.0.2_1d22f81a   Vendor: ATT
```

| 文件 | bank 0 (p2) | bank 1 (p3) | 自编译 |
|---|---|---|---|
| `zImage` | 4,211,200 `64a8b1f4` | 4,211,200 `491aad07` | 4,211,200 `7c98569e` |
| `dt.dtb` | 33,014 `23c76bca` | 33,014 `23c76bca` | **`23c76bca` 相同** |
| `rootfs.cpio.uboot` | 53,247,113 | 52,847,794 | 46,168,229 |

三个内核大小一致（同一份 4.14.54 源码），哈希不同只是构建时间不同；
设备树三家逐字节相同。

p3 分区 364 MiB，已用 57 MiB，可用 285 MiB —— 46 MB 的 rootfs 装得下。
