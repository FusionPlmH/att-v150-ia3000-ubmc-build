fatload mmc 0:1 0x88000000 /dt.dtb && fatload mmc 0:1 0x82000000 /zImage && fatload mmc 0:1 0x88080000 /rootfs.cpio.uboot && setenv bootargs console=ttyS0,115200n8 root=/dev/ram0 rw loglevel=7 ubmc_boot=sd && bootz 0x82000000 0x88080000 0x88000000
ext2load mmc 1:2 0x88000000 /dt.dtb
ext2load mmc 1:2 0x82000000 /zImage
ext2load mmc 1:2 0x88080000 /rootfs.cpio.uboot
setenv bootargs console=ttyS0,115200n8 root=/dev/ram0 rw quiet loglevel=2 ubmc_boot=emmc
bootz 0x82000000 0x88080000 0x88000000
