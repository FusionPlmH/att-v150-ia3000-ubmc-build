预编译产物
==========

`ubmc_ipmi_lan` 是 `scripts/ipmi-lan/ubmc_ipmi_lan.c` 的静态交叉编译结果，
由 `scripts/ipmi-lan/build-cross.sh` 生成（WSL + `gcc-arm-linux-gnueabihf` +
`libssl-dev:armhf`）。

为什么要把它放进仓库
--------------------
uBMC 的根文件系统是 ramdisk（`root=/dev/ram0`），`/root` 也在里面，所以构建机
（buildroot 工具链）一重启就没了；镜像自带 glibc 2.21，现代交叉编译器又要
2.34+。留一份静态二进制，就不必为了重打镜像再去重建工具链。

`rebuild-image.sh` 的取用顺序：
  1. `$PREBUILT_IPMI_LAN`（环境变量，可覆盖）
  2. `prebuilt/ubmc_ipmi_lan`（本文件）
  3. buildroot 交叉编译器现场编译（工具链还在时才走这条）

`*.gz` 只是往真机上 curl 传输用的压缩副本，不进仓库。

现状
----
- 静态链接，strip 后 2,488,892 B，不依赖镜像里的任何 `.so`
- `file` 认作：ELF 32-bit LSB executable, ARM, EABI5, statically linked
