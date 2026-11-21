# Push a cross-compiled ubmc_ipmi_lan onto a live box through the web RPC and
# restart it.  Used during bring-up; the persistent copy lives in the image.
#
#   . .\deploy-ipmi-lan.ps1
#   Deploy-IpmiLan -Version v12

$script:UbmcDelivery = "C:\Users\Peter\Desktop\silicom att v150\ubmc-bank1"

function Deploy-IpmiLan {
    param(
        [Parameter(Mandatory)][string]$Version,
        [string]$WslBinary = "/root/ipmi-build/ubmc_ipmi_lan"
    )

    $local = Join-Path $script:UbmcDelivery "ubmc_ipmi_lan.$Version"
    $wsl = "/mnt/c/Users/Peter/Desktop/silicom att v150/ubmc-bank1/ubmc_ipmi_lan.$Version"
    & wsl.exe -d Ubuntu -u root -- bash -lc "cp -f '$WslBinary' '$wsl'" | Out-Null
    if (-not (Test-Path $local)) { throw "copy out of WSL failed: $local" }

    $md5 = (Get-FileHash $local -Algorithm MD5).Hash.ToLower()
    $b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($local))
    $chunk = 7000
    $n = [math]::Ceiling($b64.Length / $chunk)

    Invoke-UbmcShell -Script "rm -f /tmp/d.b64" | Out-Null
    for ($i = 0; $i -lt $n; $i++) {
        $s = $i * $chunk
        $part = $b64.Substring($s, [Math]::Min($chunk, $b64.Length - $s))
        Invoke-UbmcShell -Script "printf '%s' '$part' >> /tmp/d.b64" | Out-Null
    }

    $script = @"
set -e
D=/usr/sbin/ubmc_ipmi_lan
got=`$(base64 -d /tmp/d.b64 | md5sum | cut -d' ' -f1)
echo "pushed    = `$got"
[ "`$got" = "$md5" ] || { echo "MD5 MISMATCH, aborting"; exit 1; }
killall -q ubmc_ipmi_lan 2>/dev/null || true
sleep 1
base64 -d /tmp/d.b64 > `${D}.new && chmod 755 `${D}.new && mv -f `${D}.new `$D
rm -f /tmp/d.b64
echo "installed = `$(md5sum `$D | cut -d' ' -f1)"
: > /var/log/ipmi-lan.log
IPMI_LAN_DEBUG=1 setsid `$D -d >/tmp/ipmi-start.log 2>&1 </dev/null || true
sleep 2
echo "pid       = `$(pidof ubmc_ipmi_lan || echo NONE)"
tail -5 /var/log/ipmi-lan.log
"@
    return Invoke-UbmcShell -Script $script
}
