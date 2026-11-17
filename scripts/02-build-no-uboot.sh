#!/bin/bash
# 02-build-no-uboot.sh —— 跳过 u-boot，只构建 kernel + rootfs
#
#   sudo ./02-build-no-uboot.sh
#
# 什么时候用：01-build.sh 卡在 u-boot（源码包 404 或编译错误），但你想先拿到
# 一份能 chroot 进去测试的 rootfs。关掉 BR2_TARGET_UBOOT 后 u-boot 完全不参与，
# zImage + rootfs 照常产出。
#
# 会先把 .config 备份成 .config.before-no-uboot，可以来回切。

set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
# shellcheck source=env.sh
. "$HERE/env.sh"
# shellcheck source=chroot-lib.sh
. "$HERE/chroot-lib.sh"

CFG="$UBMC_BUILDROOT/.config"

case "${1:-}" in
-h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac

ubmc_need_root
[ -f "$CFG" ] || { echo "没有 $CFG —— 先跑一次 01-build.sh 让它生成配置" >&2; exit 1; }
mkdir -p "$UBMC_WORK"

echo '=== 关闭 BR2_TARGET_UBOOT ==='
[ -f "$CFG.before-no-uboot" ] || cp -p "$CFG" "$CFG.before-no-uboot"
sed -i 's/^BR2_TARGET_UBOOT=y$/# BR2_TARGET_UBOOT is not set/' "$CFG"
grep -n '^BR2_TARGET_UBOOT\|^# BR2_TARGET_UBOOT' "$CFG" | head -3

echo
echo "=== 继续构建 $(date -Is) ===" | tee -a "$UBMC_LOG"
ubmc_mount_chroot

set +e
ubmc_make 2>&1 | tee -a "$UBMC_LOG" | tail -40
rc=${PIPESTATUS[0]}
set -e

echo "=== build exited rc=$rc $(date -Is) ===" | tee -a "$UBMC_LOG"

if [ "$rc" -eq 0 ]; then
	echo
	echo '=== 产出物 ==='
	ls -lh "$UBMC_BUILDROOT/output/images/" 2>/dev/null || true
	echo
	echo '把 rootfs 传到 BMC 上测：见 chroot-and-recovery.md'
else
	echo "失败，完整日志：$UBMC_LOG" >&2
	exit "$rc"
fi
