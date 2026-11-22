# 主构建：Buildroot

[← 返回总览](README.md)

---

## 为什么用 Debian stretch chroot

uBMC 是 2018 年的代码，要跑在 **glibc 2.24 / gcc 6** 那一代工具链上。
直接用现代的 WSL（Debian 12 / Ubuntu 24.04，gcc 12+、glibc 2.36+）编译，
会遇到大量过时语法和 ABI 问题。

所以做法是：**在一个 Debian stretch chroot 里编译**。
宿主系统（WSL、PVE、VM 都行）只负责提供内核和文件系统。

```
宿主（Debian 12 / Ubuntu / WSL，任意新版）
└── $UBMC_CHROOT  (默认 ~/ubmc-chroot)     ← debootstrap 出来的 Debian 9
    └── /srv/ubmc                          ← bind mount 到 $UBMC_SRC
        └── buildroot/                     ← 在这里 make
```

`chroot` 的好处：不污染宿主系统；`$UBMC_IN_CHROOT` 是 bind mount，指向宿主的
源码目录，用任何编辑器改起来都方便。

---

## 第 1 步：建 chroot 环境

脚本：[scripts/00-setup.sh](scripts/00-setup.sh)

```sh
sudo ./scripts/00-setup.sh            # 建 + 装依赖（幂等，可以反复跑）
sudo ./scripts/00-setup.sh --check    # 只体检，不改任何东西
sudo ./scripts/00-setup.sh --recreate # 删掉重建（源码不动）
```

它做这几件事：

1. **debootstrap** 一个 Debian stretch 到 `$UBMC_CHROOT`（默认 `~/ubmc-chroot`）
2. 把 apt 源换成 `archive.debian.org` —— stretch 2022-06 已 EOL，
   官方源已下线，且 `Release` 文件过期，所以要关掉 `Check-Valid-Until`
3. 装 20 个构建依赖（build-essential / bison / flex / dtc / libssl-dev …）
4. 挂伪文件系统 + 把源码 bind 进 `$UBMC_IN_CHROOT`（默认 `/srv/ubmc`）

跑完自检会打印：

```
  Debian         9.13
  gcc            gcc (Debian 6.3.0-18+deb9u1) 6.3.0
  make           GNU Make 4.1
  dtc            Version: DTC 1.4.1
  源码可见        buildroot product ...
```

`os` 必须是 9.x。用新发行版编出来的东西在 BMC 上跑不了
（`GLIBC_2.28 not found` —— BMC 上是 glibc 2.24）。

> 路径都在 [scripts/env.sh](scripts/env.sh) 里，复制成 `env.local.sh` 改就行。

---

## 第 2 步：配置

源码根目录：

```
product/ubmc/configs/
├── ubmc_m.config              ← M 版 = ATT-V150 用的（普通固件）
├── ubmc_m_install.config      ← 安装盘版（口令是 root）
└── ubmc_t.config              ← T 版
```

M 版里两条关键配置（也是"默认口令"的来源）：

```ini
BR2_TARGET_ENABLE_ROOT_LOGIN=y
BR2_TARGET_GENERIC_ROOT_PASSWD="<pick-one>"
```

> **`root` 的口令就是在这里写死的。** AT&T 出厂固件只改了 `is_admin`，
> `root` 一直保持这个值。

---

## 第 3 步：构建

脚本：[scripts/01-build.sh](scripts/01-build.sh)

```sh
sudo ./scripts/01-build.sh            # 完整构建
sudo ./scripts/01-build.sh -j8        # 指定并行度
sudo ./scripts/01-build.sh uboot-custom   # 只编某一个包
```

它内部就这一句：

```sh
chroot "$UBMC_CHROOT" /bin/bash -c \
  "cd /srv/ubmc/buildroot && export FORCE_UNSAFE_CONFIGURE=1 && exec make"
```

三个要点：

| 要点 | 为什么 |
|---|---|
| `FORCE_UNSAFE_CONFIGURE=1` | 很多老包的 `configure` 拒绝以 root 运行，这个变量是官方逃生门 |
| `exec make` | 让 make 成为 PID 1，`kill` 能直接打到它 |
| 输出同时 `tee` 到 `build.log` | Buildroot 输出极长，出问题时只看尾部不够 |

跑完看标识：

```
=== build exited rc=0 at <时间> ===
```

**Buildroot 是增量的** —— 改过配置或代码之后，同一条命令再跑即可，不会从头来。

---

## ★ 那个卡了很久的 bug：`HOSTCFLAGS` 拼错

### 症状

构建在编译内核的 `scripts/dtc/fdt_overlay.c` 时失败，报 libfdt 相关错误。
更麻烦的是它**有时不报错**，而是悄悄用了系统的旧头文件 `/usr/include/libfdt.h`
—— 产出的固件看起来正常，实际行为诡异。

### 根因

`buildroot/linux/linux.mk` 里有这么一行（在传给内核 make 的命令行上）：

```make
HOSTCFLAGS="$(HOSTCFLAGS)"
```

**Buildroot 的变量叫 `HOST_CFLAGS`**（带下划线）。`$(HOSTCFLAGS)` 是空的，
于是这行把内核自己的 `HOSTCFLAGS` **清空**了 —— 其中包含关键的
`-Iscripts/dtc/libfdt`。

丢了那个 `-I` 之后，编译器就去系统路径找 `libfdt.h`，
命中了 Debian 里那份过时的 `/usr/include/libfdt.h`（`libfdt-dev` 装的），
于是用错的头文件编出了错的东西。

### 修法

**删掉那一行。**

```sh
sed -i '/^HOSTCFLAGS="\$(HOSTCFLAGS)"$/d' buildroot/linux/linux.mk
grep -n 'HOSTCFLAGS' buildroot/linux/linux.mk   # 确认没了
```

删完之后 `build exited rc=0`，`zImage` 4.2 MB，一切正常。

> 顺带说：**别试图"修"成 `HOSTCFLAGS="$(HOST_CFLAGS)"`**。
> 那会把自己的 CFLAGS 塞进内核构建，同样会打架。删掉最干净 ——
> 内核知道自己的 `HOSTCFLAGS` 该是什么。

### 教训

这类"拼错变量名导致静默降级"的 bug 最难查，因为：

1. 它不总是报错 —— 能找到同名头文件就编过了
2. 报错的地方（`fdt_overlay.c`）离真正的原因（`linux.mk` 里一行）很远
3. 产出的固件"看起来能用"，问题留到运行时才炸

查法：怀疑某个 `-I` 没生效时，`make V=1` 看实际编译命令行，
对比 `CFLAGS` / `HOSTCFLAGS` 是否为空。

---

## 第 4 步：产出物

`buildroot/output/images/`：

| 文件 | 大小 | 用途 |
|---|---|---|
| `zImage` | ~4.2 MB | ARM 内核 |
| `rootfs.cpio.uboot` | — | **正式启动用这个**（initramfs，`root=/dev/ram0`） |
| `rootfs.ext2` | — | 同一 rootfs 的磁盘镜像 |
| `rootfs.tar` | — | 打包形式 |
| `dt.dtb` | ~32 KB | 设备树 |

### rootfs 是 cpio，不是分区

正式启动的 bootargs 是：

```
console=ttyS0,115200n8 root=/dev/ram0 rw quiet loglevel=2
```

`root=/dev/ram0` → 根文件系统是 **initramfs（cpio）**，每次启动解压到内存。
所以：

- **改 `/etc` 下的东西不会持久** —— 重启就从 cpio 重建
- **不能用 `ext4write` 改 `/etc/shadow`**（网上很多 ARM 设备教程这么干，这里不适用）
- 要改持久配置，改 `/config`（mgmtd 的配置库，在 eMMC 上）

---

## 关掉 u-boot 包继续构建

如果只是改了用户态、不想被 u-boot 的编译问题挡住：

```sh
sudo ./scripts/02-build-no-uboot.sh
```

它做的事：

```sh
cp -p buildroot/.config buildroot/.config.before-no-uboot
sed -i 's/^BR2_TARGET_UBOOT=y$/# BR2_TARGET_UBOOT is not set/' buildroot/.config
# 然后重新 make
```

**前提是 bootloader 不需要变**。要用自己编的 U-Boot，见 [uboot.md](uboot.md)。

---

## 常见问题

| 现象 | 处理 |
|---|---|
| `configure: error: you should not run configure as root` | `export FORCE_UNSAFE_CONFIGURE=1` |
| 下载卡住 / 404 | 先确认 chroot 里 `resolv.conf` 通。老包的下载地址很多已经死了 |
| 改了配置但没生效 | Buildroot 会在 `output/build/` 缓存，改 `.config` 后要 `make` 重跑；改源码要 `make <pkg>-rebuild` |
| 想从头来 | `make clean`（保留下载缓存）或 `rm -rf output`（全清，会重新下载） |
