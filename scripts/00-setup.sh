#!/bin/bash
# 00-setup.sh —— 从零准备 uBMC 构建环境
#
#   sudo ./00-setup.sh              建 chroot + 装依赖（已有则只补挂载和缺包）
#   sudo ./00-setup.sh --recreate   删掉 chroot 重建（源码不动）
#        ./00-setup.sh --check      只体检，不改任何东西（不需要 root）
#        ./00-setup.sh --umount     卸载 chroot 的挂载点
#        ./00-setup.sh -h
#
# 为什么用 Debian stretch：uBMC 出厂的 glibc 是 2.24，内核 4.14。
# 用新发行版编出来的东西在 BMC 上跑不了（GLIBC_2.28 not found）。
# stretch 已于 2022-06 EOL，所以源指向 archive.debian.org。

set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
# shellcheck source=env.sh
. "$HERE/env.sh"
# shellcheck source=chroot-lib.sh
. "$HERE/chroot-lib.sh"

RECREATE=0
CHECK=0
UMOUNT=0

usage() {
	sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
	cat <<EOF

当前配置：
$(ubmc_show_env)
EOF
}

for a in "$@"; do
	case "$a" in
	--recreate) RECREATE=1 ;;
	--check)    CHECK=1 ;;
	--umount)   UMOUNT=1 ;;
	-h|--help)  usage; exit 0 ;;
	*) echo "未知参数：$a（-h 看用法）" >&2; exit 1 ;;
	esac
done

# chroot 里要装的包。Buildroot 很少用系统库，但这些是 configure 阶段查的。
PKGS=(
	build-essential bison flex gettext texinfo unzip bc rsync cpio
	python3 wget curl git patch perl file bzip2 xz-utils
	libncurses5-dev libssl-dev device-tree-compiler
	autoconf automake libtool pkg-config ca-certificates
)

say() { printf '\n=== %s ===\n' "$*"; }
ok()  { printf '  [ok] %s\n' "$*"; }
bad() { printf '  [!!] %s\n' "$*"; }

# ---------------------------------------------------------------- 体检
if [ "$CHECK" -eq 1 ]; then
	say "配置"
	ubmc_show_env

	say "宿主"
	printf '  %-22s %s\n' '内核'     "$(uname -r)"
	printf '  %-22s %s\n' '架构'     "$(uname -m)"
	printf '  %-22s %s\n' 'Debian 系' "$( [ -f /etc/debian_version ] && echo yes || echo 'no (这脚本要在 Debian/Ubuntu/WSL 上跑)' )"
	for c in debootstrap chroot git; do
		printf '  %-22s %s\n' "$c" "$(command -v "$c" || echo '缺失')"
	done

	say "源码仓库"
	if [ -d "$UBMC_SRC" ]; then
		ok "$UBMC_SRC"
		[ -d "$UBMC_BUILDROOT" ] && ok "Buildroot: $UBMC_BUILDROOT" || bad "没有 $UBMC_BUILDROOT"
		[ -x "$UBMC_SRC/product/ubmc/scripts/build.sh" ] \
			&& ok 'product/ubmc/scripts/build.sh' \
			|| printf '  [--] product/ubmc/scripts/build.sh 不存在（不一定需要）\n'
	else
		bad "没有 $UBMC_SRC —— 先 git clone https://github.com/silicom-ltd/uBMC $UBMC_SRC"
	fi

	say "chroot"
	if [ -d "$UBMC_CHROOT/etc" ]; then
		ok "$UBMC_CHROOT"
		printf '  %-22s %s\n' 'Debian 版本' "$(cat "$UBMC_CHROOT/etc/debian_version" 2>/dev/null || echo '?')"
		printf '  %-22s %s\n' 'gcc' "$(chroot "$UBMC_CHROOT" gcc --version 2>/dev/null | head -1 || echo '缺失')"
	else
		bad "没有 $UBMC_CHROOT —— 跑 sudo $0"
	fi

	say "挂载状态"
	if mountpoint -q "$UBMC_CHROOT$UBMC_IN_CHROOT" 2>/dev/null; then ok '源码已 bind'; else bad '源码未 bind'; fi
	for m in proc sys dev dev/pts; do
		mountpoint -q "$UBMC_CHROOT/$m" 2>/dev/null && ok "$m" || bad "$m 未挂载"
	done
	exit 0
fi

if [ "$UMOUNT" -eq 1 ]; then
	ubmc_need_root
	say "卸载"
	ubmc_umount_chroot
	exit 0
fi

# ---------------------------------------------------------------- 建环境
ubmc_need_root

# 这套流程只对 Debian 系成立：debootstrap + apt。
if [ ! -f /etc/debian_version ]; then
	cat >&2 <<'EOF'

这个脚本只能在 Debian / Ubuntu / Proxmox VE 上跑 —— 它用 debootstrap 建
Debian stretch chroot，用 apt 装依赖。

别的发行版（RHEL / Fedora / Arch）上有两条路：
  1. 开一台 Debian 12 的虚拟机（或用 WSL），在里面跑
  2. 手工建一个 stretch rootfs（debootstrap 在多数发行版仓库里也有），
     然后把 UBMC_CHROOT 指过去，跳过本脚本，直接跑 01-build.sh

EOF
	exit 1
fi

say "配置"
ubmc_show_env

say "宿主依赖"
if ! command -v debootstrap >/dev/null 2>&1; then
	echo '  安装 debootstrap ...'
	apt-get update -qq
	DEBIAN_FRONTEND=noninteractive apt-get install -y -qq debootstrap
fi
ok "debootstrap $(debootstrap --version 2>/dev/null | head -1 || echo '')"

[ -d "$UBMC_SRC" ] || {
	cat >&2 <<EOF

  源码仓库不存在：$UBMC_SRC

  git clone https://github.com/silicom-ltd/uBMC "$UBMC_SRC"

  或者用别的路径：UBMC_SRC=/path/to/uBMC sudo -E $0

EOF
	exit 1
}
ok "源码 $UBMC_SRC"

if [ "$RECREATE" -eq 1 ] && [ -d "$UBMC_CHROOT" ]; then
	say "删除旧 chroot（--recreate）"
	ubmc_umount_chroot
	rm -rf "$UBMC_CHROOT"
	ok "已删"
fi

if [ -d "$UBMC_CHROOT/etc" ]; then
	say "chroot 已存在，跳过 debootstrap"
	ok "$UBMC_CHROOT"
else
	say "debootstrap $UBMC_DISTRO -> $UBMC_CHROOT"
	mkdir -p "$UBMC_CHROOT" "$UBMC_WORK"
	# archive.debian.org 的 Release 已过期，debootstrap 的 GPG 校验必然失败，
	# 所以这里用 --no-check-gpg：不校验 Release 签名，但仍校验每个 .deb 的 md5。
	debootstrap --no-check-gpg --variant=minbase \
		--arch="$(dpkg --print-architecture)" \
		"$UBMC_DISTRO" "$UBMC_CHROOT" "$UBMC_MIRROR" 2>&1 | tail -12
	ok 'debootstrap 完成'
fi

say "配置 chroot 的 apt"
# stretch 已 EOL：源换成 archive，并且不再检查 Valid-Until
cat >"$UBMC_CHROOT/etc/apt/sources.list" <<EOF
deb $UBMC_MIRROR $UBMC_DISTRO main
deb $UBMC_MIRROR $UBMC_DISTRO-updates main
deb http://archive.debian.org/debian-security $UBMC_DISTRO/updates main
EOF
cat >"$UBMC_CHROOT/etc/apt/apt.conf.d/99no-valid-until" <<'EOF'
Acquire::Check-Valid-Until "false";
Acquire::AllowInsecureRepositories "true";
EOF

ubmc_mount_chroot >/dev/null
chroot "$UBMC_CHROOT" /bin/bash -c 'apt-get update -qq' 2>&1 | tail -3 || true
ok 'apt update'

say "安装构建依赖（chroot 内，${#PKGS[@]} 个包）"
# shellcheck disable=SC2016
chroot "$UBMC_CHROOT" /bin/bash -c \
	"DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends ${PKGS[*]}" \
	2>&1 | tail -8
ok '依赖装好'

say "自检"
ubmc_mount_chroot
printf '  %-14s %s\n' 'Debian' "$(cat "$UBMC_CHROOT/etc/debian_version" 2>/dev/null)"
printf '  %-14s %s\n' 'gcc'    "$(chroot "$UBMC_CHROOT" gcc --version 2>/dev/null | head -1)"
printf '  %-14s %s\n' 'make'   "$(chroot "$UBMC_CHROOT" make --version 2>/dev/null | head -1)"
printf '  %-14s %s\n' 'dtc'    "$(chroot "$UBMC_CHROOT" dtc --version 2>/dev/null | head -1)"
printf '  %-14s %s\n' '源码可见' "$(chroot "$UBMC_CHROOT" ls "$UBMC_IN_CHROOT" 2>/dev/null | head -3 | tr '\n' ' ')"

cat <<EOF

=== 下一步 ===
  ./01-build.sh               完整构建（u-boot + kernel + rootfs）
  ./02-build-no-uboot.sh      只想先出 rootfs，跳过 u-boot
  ./03-fix-uboot-tarball.sh   单独重建 u-boot 的源码包（01 在 u-boot 处失败时）

日志会写到 $UBMC_LOG
EOF
