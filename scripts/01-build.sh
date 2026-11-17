#!/bin/bash
# 01-build.sh —— 完整构建 uBMC 固件（u-boot + kernel + rootfs）
#
#   sudo ./01-build.sh                构建，日志同时写 $UBMC_LOG
#   sudo ./01-build.sh -j8            指定并行度
#   sudo ./01-build.sh uboot-custom   只编某一个包（断点续跑用）
#        ./01-build.sh --env          只打印当前配置
#        ./01-build.sh -h
#
# Buildroot 是增量的：中断后重跑同一条命令就行，已编好的不会重编。

set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
# shellcheck source=env.sh
. "$HERE/env.sh"
# shellcheck source=chroot-lib.sh
. "$HERE/chroot-lib.sh"

JOBS=""
TARGETS=()

usage() {
	sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
	printf '\n当前配置：\n'
	ubmc_show_env
}

while [ $# -gt 0 ]; do
	case "$1" in
	-j)       JOBS="${2:?-j 需要一个数字}"; shift ;;
	-j*)      JOBS="${1#-j}" ;;
	--env)    ubmc_show_env; exit 0 ;;
	-h|--help) usage; exit 0 ;;
	-*)       echo "未知参数：$1" >&2; exit 1 ;;
	*)        TARGETS+=("$1") ;;
	esac
	shift
done

ubmc_need_root
mkdir -p "$UBMC_WORK"

echo "=== build start $(date -Is) ===" | tee -a "$UBMC_LOG"
ubmc_mount_chroot

set +e
# shellcheck disable=SC2086
ubmc_make ${JOBS:+-j"$JOBS"} "${TARGETS[@]}" 2>&1 | tee -a "$UBMC_LOG"
rc=${PIPESTATUS[0]}
set -e

echo "=== build exited rc=$rc at $(date -Is) ===" | tee -a "$UBMC_LOG"

if [ "$rc" -ne 0 ]; then
	cat >&2 <<EOF

构建失败（rc=$rc）。按报错对照 README 的坑表：
  fdt_overlay.c / libfdt 报错   -> buildroot/linux/linux.mk 里 HOSTCFLAGS 那行拼错，删掉它
  u-boot 源码包 404             -> 跑 ./03-fix-uboot-tarball.sh
  只想先出 rootfs               -> 跑 ./02-build-no-uboot.sh

完整日志：$UBMC_LOG
EOF
	exit "$rc"
fi

echo
echo '=== 产出物 ==='
ls -lh "$UBMC_BUILDROOT/output/images/" 2>/dev/null ||
	echo "  没有 output/images —— 检查构建是否真的完成"
