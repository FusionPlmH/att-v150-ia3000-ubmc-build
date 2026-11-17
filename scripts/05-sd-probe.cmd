echo ""
echo "=============================================="
echo "  SD U-BOOT RUNNING - diagnostics below"
echo "=============================================="
mmc list
echo "--- mmc 0 (SD) ---"
mmc dev 0
mmc info
mmc part
echo "--- mmc 1 (eMMC?) ---"
mmc dev 1
mmc info
mmc part
echo "--- mmc 2 ---"
mmc dev 2
mmc info
mmc part
echo "=============================================="
echo "  DIAGNOSTICS DONE"
echo "=============================================="
