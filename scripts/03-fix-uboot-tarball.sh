#!/bin/bash
# 03-fix-uboot-tarball.sh —— 重建 Buildroot 要的那个已失效的 u-boot 源码包
#
#   sudo ./03-fix-uboot-tarball.sh
#
# 背景：Buildroot 的配置里写的是 u-boot commit 590c7d7fe1，这是 Silicom/Meganet
# 的私有 commit，上游 git.u-boot-project.org 和 GitHub 镜像都找不到。
# 做法：取官方 v2017.01 tag，依次打上源码仓库里的 11 个 vendor 补丁，
# 打成 Buildroot 期望的文件名，放进 dl/ 目录。
#
# 不需要 root 权限（只动 $UBMC_SRC 和 $UBMC_WORK）。

set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
# shellcheck source=env.sh
. "$HERE/env.sh"

DL="$UBMC_BUILDROOT/dl"
PATCHES="$UBMC_SRC/product/ubmc/hardware/ti/uboot/patches"
NAME="$UBMC_UBOOT_NAME"
SRC_DIR="$UBMC_WORK/uboot-src"
TARBALL="$DL/$NAME.tar.gz"

UPSTREAM=(
	https://github.com/TexasInstruments/ti-u-boot.git
	https://git.ti.com/ti-u-boot/ti-u-boot.git
)
UPSTREAM_BRANCH=ti-u-boot-2017.01

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; }
for a in "$@"; do
	case "$a" in -h|--help) usage; exit 0 ;; esac
done

echo "=== 配置 ==="
ubmc_show_env
echo "  补丁目录   $PATCHES"
echo "  输出       $TARBALL"

[ -d "$UBMC_SRC" ] || { echo "源码仓库不存在：$UBMC_SRC" >&2; exit 1; }
[ -d "$PATCHES" ] || { echo "补丁目录不存在：$PATCHES" >&2; exit 1; }
mkdir -p "$DL" "$UBMC_WORK"

echo
echo '=== 1. 取 TI ti-u-boot-2017.01 分支 ==='
# ★ 必须是 TI 的分支，不能是 vanilla u-boot：
#   - vanilla v2017.01 缺 include/environment/ti/mmc.h（v2017.05 才加）
#   - vanilla v2017.09 结构体改名（cm0ioctl 等），11 个补丁上下文对不上
#   - TI 的 ti-u-boot-2017.01 版本号写 2017.01、有 mmc.h、有 dpll_ddr3_303MHz，
#     且保留 cm0ioctl —— 正是 Silicom 打补丁时用的基线
rm -rf "$SRC_DIR"
for url in "${UPSTREAM[@]}"; do
	echo "  尝试 $url ($UPSTREAM_BRANCH)"
	if timeout 900 git clone --depth 1 --branch "$UPSTREAM_BRANCH" "$url" "$SRC_DIR" 2>/dev/null; then
		echo "  成功"
		break
	fi
	rm -rf "$SRC_DIR"
done
[ -d "$SRC_DIR/.git" ] || { echo '所有上游源都失败，检查网络' >&2; exit 1; }
( cd "$SRC_DIR" && echo "  当前: $(git log --oneline -1)" )

echo
echo '=== 2. 依次打 vendor 补丁 ==='
FAILED=""
count=0
for p in "$PATCHES"/uboot-*.patch; do
	[ -e "$p" ] || continue
	count=$((count + 1))
	printf '  %-44s ' "$(basename "$p")"
	if ( cd "$SRC_DIR" && patch -p1 --forward --batch <"$p" >/tmp/ubmc-patch.log 2>&1 ); then
		echo OK
	elif grep -q 'Reversed (or previously applied)' /tmp/ubmc-patch.log; then
		echo '已应用过'
	else
		echo FAILED
		FAILED="$FAILED $(basename "$p")"
		sed -n '1,4p' /tmp/ubmc-patch.log | sed 's/^/         /'
	fi
done
echo "  共 $count 个补丁"
if [ -n "$FAILED" ]; then
	cat >&2 <<EOF

  !! 这些补丁没打上：$FAILED

  先确认源码仓库版本和补丁是同期的：
      cd $UBMC_SRC && git log --oneline -1

  补丁上下文对不上通常意味着补丁是给别的 u-boot 版本写的。
  硬要继续的话，手工在 $SRC_DIR 里修好，然后重跑本脚本的第 3、4 步。
EOF
	exit 1
fi

echo
echo '=== 3. 打成 Buildroot 期望的名字 ==='
rm -rf "$UBMC_WORK/$NAME"
mv "$SRC_DIR" "$UBMC_WORK/$NAME"
tar czf "$TARBALL" -C "$UBMC_WORK" --exclude=.git "$NAME"
ls -lh "$TARBALL"

echo
echo '=== 4. 清掉 Buildroot 里半成品的 u-boot 构建目录 ==='
rm -rf "$UBMC_BUILDROOT/output/build/uboot-custom"
echo '  已清'

cat <<EOF

=== 完成 $(date -Is) ===
现在重跑完整构建：
  sudo ./01-build.sh
EOF
