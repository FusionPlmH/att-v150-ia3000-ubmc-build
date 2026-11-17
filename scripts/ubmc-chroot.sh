#!/bin/sh
# ubmc-chroot.sh —— 在 BMC 上直接把一个 rootfs 目录 chroot 起来测（不重启、不动 eMMC）
#
#   ubmc-chroot.sh                 进默认 rootfs 的交互 shell
#   ubmc-chroot.sh -l              列出 /var/images 下所有 rootfs 候选
#   ubmc-chroot.sh /var/images/my  指定 rootfs 目录
#   ubmc-chroot.sh /var/images/my "uname -m; ls /"   执行一条命令就退出
#   ubmc-chroot.sh -u [rootfs]     卸载伪文件系统（退出后残留时用）
#
# rootfs 必须是 armv7l (armhf) + glibc <= 2.24，才能跑在 BMC 自己的 4.14 内核上。
# 判据：file rootfs/bin/busybox 或 readelf -V rootfs/lib/libc.so.6

R=${UBMC_ROOTFS:-/var/images/testroot}

case "$1" in
-l|--list)
	echo "/var/images 下的候选："
	for d in /var/images/*/; do
		[ -x "$d/bin/sh" ] || [ -x "$d/bin/busybox" ] || continue
		printf '  %-40s %s\n' "$d" "$(du -sh "$d" 2>/dev/null | cut -f1)"
	done
	exit 0
	;;
-u|--umount)
	[ -n "${2:-}" ] && R="$2"
	for m in run dev sys proc; do umount "$R/$m" 2>/dev/null; done
	echo "unmounted $R"
	exit 0
	;;
-h|--help)
	sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
	exit 0
	;;
esac

[ -n "${1:-}" ] && R="$1" && shift
[ -d "$R" ] || {
	echo "没有这个 rootfs：$R" >&2
	echo "用 $0 -l 看有哪些" >&2
	exit 1
}
[ -x "$R/bin/sh" ] || {
	echo "$R/bin/sh 不存在或不可执行 —— 这不像一个 rootfs" >&2
	exit 1
}

mount -t proc  proc  "$R/proc" 2>/dev/null
mount -t sysfs sysfs "$R/sys"  2>/dev/null
mount --bind /dev  "$R/dev"  2>/dev/null
[ -d "$R/run" ] && mount --bind /run "$R/run" 2>/dev/null

if [ -n "${*:-}" ]; then
	exec chroot "$R" /bin/sh -c "$*"
fi

echo "进入 $R（exit 退出）"
exec chroot "$R" /bin/sh
