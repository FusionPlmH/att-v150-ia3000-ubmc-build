#!/bin/sh
# chroot 挂载 / 卸载的公用函数。被 00/01/02/03 引用，不单独运行。
#
# 幂等：重复调用不会重复挂载。

ubmc_need_root() {
	[ "$(id -u)" -eq 0 ] || {
		echo "需要 root。用：sudo $0 $*" >&2
		exit 1
	}
}

# 把 $UBMC_SRC bind 到 chroot 的 $UBMC_IN_CHROOT，并挂上伪文件系统。
# 返回非 0 表示 chroot 还没建（先跑 00-setup.sh）。
ubmc_mount_chroot() {
	R="$UBMC_CHROOT"
	[ -d "$R" ] || { echo "chroot 不存在：$R —— 先跑 00-setup.sh" >&2; return 1; }

	mkdir -p "$R$UBMC_IN_CHROOT"
	mountpoint -q "$R$UBMC_IN_CHROOT" 2>/dev/null ||
		{ mount --bind "$UBMC_SRC" "$R$UBMC_IN_CHROOT" && echo "  bind $UBMC_SRC"; }

	mountpoint -q "$R/proc"    || { mount -t proc   proc   "$R/proc";    echo "  proc"; }
	mountpoint -q "$R/sys"     || { mount -t sysfs  sysfs  "$R/sys";     echo "  sysfs"; }
	mountpoint -q "$R/dev"     || { mount --bind /dev "$R/dev";          echo "  dev"; }
	mkdir -p "$R/dev/pts"
	mountpoint -q "$R/dev/pts" || { mount -t devpts devpts "$R/dev/pts"; echo "  devpts"; }

	if [ -f /etc/resolv.conf ]; then
		cp -f /etc/resolv.conf "$R/etc/resolv.conf" 2>/dev/null && echo "  resolv.conf OK"
	fi
	return 0
}

ubmc_umount_chroot() {
	R="$UBMC_CHROOT"
	[ -d "$R" ] || return 0
	for m in dev/pts dev sys proc "$UBMC_IN_CHROOT"; do
		mountpoint -q "$R/$m" 2>/dev/null && { umount "$R/$m" && echo "  umount $m"; }
	done
	return 0
}

# 在 chroot 里进 Buildroot 目录执行 make。额外参数原样传下去。
#   ubmc_make              -> make
#   ubmc_make -j8          -> make -j8
#   ubmc_make uboot-custom -> 只编某个包
ubmc_make() {
	chroot "$UBMC_CHROOT" /bin/bash -c \
		"cd $UBMC_IN_CHROOT/$UBMC_BUILDROOT_REL && export FORCE_UNSAFE_CONFIGURE=1 && exec make $*"
}
