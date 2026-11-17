echo "=== UBMC boot0: SD-first wrapper ==="
setenv silc_sd 1
if fatload mmc 0:1 0x88000000 /dt.dtb; then echo "  [SD] dt.dtb ok"; else setenv silc_sd 0; fi
if fatload mmc 0:1 0x82000000 /zImage; then echo "  [SD] zImage ok"; else setenv silc_sd 0; fi
if fatload mmc 0:1 0x88080000 /rootfs.cpio.uboot; then echo "  [SD] rootfs ok"; else setenv silc_sd 0; fi
if test ${silc_sd} = 1; then
echo "=== UBMC: BOOTING FROM SD CARD ==="
setenv bootargs console=ttyS0,115200n8 root=/dev/ram0 rw quiet loglevel=2 ubmc_boot=sd
bootz 0x82000000 0x88080000 0x88000000
fi
echo "=== UBMC: SD unusable, falling back to eMMC bank 0 ==="
ext2load mmc ${mmcdev}:2 0x88000000 /dt.dtb
ext2load mmc ${mmcdev}:2 0x82000000 /zImage
ext2load mmc ${mmcdev}:2 0x88080000 /rootfs.cpio.uboot
setenv bootargs console=ttyS0,115200n8 root=/dev/ram0 rw quiet loglevel=2 ubmc_boot=emmc
bootz 0x82000000 0x88080000 0x88000000
