#!/bin/sh
# ubmc-build 共用配置 —— 所有 0x-*.sh 都会自动 source 这个文件。
#
# 三种改法，优先级从低到高：
#   1. 什么都不做，用下面的默认值（都在 $HOME 下，不需要 root 权限）
#   2. 复制成 env.local.sh 再改 —— 脚本会自动优先读它
#   3. 命令行临时覆盖：UBMC_CHROOT=/srv/stretch ./01-build.sh
#
# 目录含义：
#   UBMC_SRC      uBMC 源码仓库（git clone https://github.com/silicom-ltd/uBMC）
#   UBMC_CHROOT   Debian stretch chroot 根目录（00-setup.sh 建）
#   UBMC_WORK     日志和中间产物

_ubmc_here=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || _ubmc_here=.

# shellcheck source=/dev/null
[ -f "$_ubmc_here/env.local.sh" ] && . "$_ubmc_here/env.local.sh"

: "${UBMC_SRC:=$HOME/uBMC}"
: "${UBMC_CHROOT:=$HOME/ubmc-chroot}"
: "${UBMC_WORK:=$HOME/ubmc-work}"

# 构建相关
: "${UBMC_BUILDROOT_REL:=buildroot}"          # 源码仓库里 Buildroot 的相对路径
: "${UBMC_DISTRO:=stretch}"                    # chroot 用哪个 Debian 版本
: "${UBMC_MIRROR:=http://archive.debian.org/debian}"   # stretch 已 EOL，只能走 archive
: "${UBMC_UBOOT_NAME:=u-boot-2017.01+gitAUTOINC+590c7d7fe1-g590c7d7fe1}"

UBMC_BUILDROOT="$UBMC_SRC/$UBMC_BUILDROOT_REL"
UBMC_LOG="$UBMC_WORK/build.log"

# 源码仓库挂在 chroot 里的位置。buildroot 会在这里 make。
UBMC_IN_CHROOT=/srv/ubmc

export UBMC_SRC UBMC_CHROOT UBMC_WORK UBMC_BUILDROOT UBMC_LOG \
       UBMC_DISTRO UBMC_MIRROR UBMC_UBOOT_NAME UBMC_IN_CHROOT \
       UBMC_BUILDROOT_REL

# 打印当前配置（脚本用 -v 或 --verbose 时调）
ubmc_show_env() {
	cat <<EOF
  源码仓库    UBMC_SRC      $UBMC_SRC
  Buildroot   UBMC_BUILDROOT $UBMC_BUILDROOT
  chroot      UBMC_CHROOT   $UBMC_CHROOT
  工作目录    UBMC_WORK     $UBMC_WORK
  日志        UBMC_LOG      $UBMC_LOG
  发行版      UBMC_DISTRO   $UBMC_DISTRO
  镜像源      UBMC_MIRROR   $UBMC_MIRROR
EOF
}
