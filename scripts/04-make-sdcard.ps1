#Requires -Version 5.1
<#
.SYNOPSIS
    Write the ATT-V150 BMC SD-boot card.

.DESCRIPTION
    Builds a microSD card that makes the BMC (TI AM3352) boot our
    UBMC-SD-INSTALL U-Boot instead of its eMMC firmware.

    Layout produced (belt and braces - both known AM335x conventions):

        0x00000 - 0xFFFFF   reserved / raw boot area
        0x20000 (128 KiB)  MLO          <- ROM raw-mode offset
        0x60000 (384 KiB)  u-boot.img   <- right after MLO
        1 MiB ...          FAT32 volume with MLO, u-boot.img, boot.scr, dt.dtb
                           (the layout the Silicom factory script uses)

    Requires an elevated PowerShell.  ALL DATA ON THE TARGET DISK IS ERASED.

.PARAMETER DiskNumber
    Physical disk number of the card reader.  Run with -List to see them.

.PARAMETER SourceDir
    Folder holding MLO, u-boot.img, boot.scr, dt.dtb.  Defaults to the folder
    this script lives in.

.PARAMETER List
    Only list physical disks and exit.

.EXAMPLE
    .\Make-SdCard.ps1 -List

.EXAMPLE
    .\Make-SdCard.ps1 -DiskNumber 3
#>
[CmdletBinding()]
param(
    [int]$DiskNumber = -1,
    [string]$SourceDir,
    [switch]$List
)

$ErrorActionPreference = 'Stop'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this from an elevated PowerShell (Run as Administrator).'
    }
}

function Show-Disks {
    Get-Disk | Select-Object Number, FriendlyName,
        @{n = 'SizeGB'; e = { [math]::Round($_.Size / 1GB, 2) } },
        @{n = 'Card?'; e = { if ($_.BusType -in 'USB', 'SD') { 'YES' } else { '' } } },
        PartitionStyle, BusType | Format-Table -AutoSize | Out-String -Width 160
}

if ($List -or $DiskNumber -lt 0) {
    Write-Host 'Physical disks on this machine:' -ForegroundColor Cyan
    Show-Disks | Write-Host
    Write-Host 'Re-run with -DiskNumber <n> for the card reader above.' -ForegroundColor Yellow
    return
}

Assert-Admin

if (-not $SourceDir) { $SourceDir = $PSScriptRoot }
$needed = 'MLO', 'u-boot.img', 'boot.scr', 'dt.dtb'
$missing = @($needed | Where-Object { -not (Test-Path (Join-Path $SourceDir $_)) })
if ($missing.Count) {
    Write-Host "Missing in ${SourceDir}:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" }
    Write-Host ''
    Write-Host 'These are Buildroot output artefacts. Copy them here first, or use -SourceDir:' -ForegroundColor Yellow
    Write-Host '  .\04-make-sdcard.ps1 -DiskNumber 3 -SourceDir D:\ubmc\output\images'
    throw 'Source files missing'
}

$disk = Get-Disk -Number $DiskNumber -ErrorAction Stop
Write-Host 'About to ERASE this disk:' -ForegroundColor Red
$disk | Select-Object Number, FriendlyName, @{n = 'SizeGB'; e = { [math]::Round($_.Size / 1GB, 2) } }, BusType |
    Format-Table -AutoSize | Out-String -Width 160 | Write-Host

if ($disk.BusType -notin 'USB', 'SD') {
    Write-Warning "BusType is $($disk.BusType), not USB/SD.  Make sure this really is the card reader."
}
$answer = Read-Host "Type ERASE to continue"
if ($answer -ne 'ERASE') { Write-Host 'Aborted.' -ForegroundColor Yellow; return }

$diskpart = Join-Path $env:TEMP 'ubmc-diskpart.txt'
Set-Content -Path $diskpart -Value @"
select disk $DiskNumber
clean
"@
Write-Host 'Cleaning disk...' -ForegroundColor Cyan
& diskpart /s $diskpart | Out-Null
Start-Sleep -Seconds 1

$devPath = "\\.\PhysicalDrive$DiskNumber"

if (-not ('RawDiskWriter' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class RawDiskWriter {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    public static extern IntPtr CreateFile(string name, uint access, uint share,
        IntPtr sec, uint disposition, uint flags, IntPtr templ);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool WriteFile(IntPtr h, byte[] buf, uint count, out uint written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetFilePointerEx(IntPtr h, long distance, out long newPos, uint method);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr h);
}
"@
}

function Write-RawRegion {
    param([string]$Target, [long]$Offset, [string]$File)

    $bytes = [IO.File]::ReadAllBytes($File)
    # Raw device writes must be a multiple of the sector size (512).
    $pad = (512 - ($bytes.Length % 512)) % 512
    if ($pad -gt 0) {
        $padded = New-Object byte[] ($bytes.Length + $pad)
        [Array]::Copy($bytes, $padded, $bytes.Length)
        $bytes = $padded
    }
    $handle = [RawDiskWriter]::CreateFile($Target, 0x40000000, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($handle.ToInt64() -eq -1) {
        throw ("CreateFile failed on {0}, win32 error {1}" -f $Target, [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
    try {
        $newPos = 0L
        if (-not [RawDiskWriter]::SetFilePointerEx($handle, $Offset, [ref]$newPos, 0)) {
            throw ("Seek failed, win32 error {0}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        }
        $written = 0
        if (-not [RawDiskWriter]::WriteFile($handle, $bytes, [uint32]$bytes.Length, [ref]$written, [IntPtr]::Zero)) {
            throw ("WriteFile failed, win32 error {0}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        }
        Write-Host ("  {0} -> 0x{1:X}  ({2} bytes)" -f (Split-Path $File -Leaf), $Offset, $written) -ForegroundColor Green
    }
    finally { [RawDiskWriter]::CloseHandle($handle) | Out-Null }
}

Write-Host 'Writing raw boot image (ROM offsets)...' -ForegroundColor Cyan
Write-RawRegion -Target $devPath -Offset 0x20000 -File (Join-Path $SourceDir 'MLO')
Write-RawRegion -Target $devPath -Offset 0x60000 -File (Join-Path $SourceDir 'u-boot.img')

Write-Host 'Creating FAT32 partition...' -ForegroundColor Cyan
Set-Content -Path $diskpart -Value @"
select disk $DiskNumber
create partition primary
format fs=fat32 quick label=UBMCINST
assign
exit
"@
& diskpart /s $diskpart | Out-Null
Start-Sleep -Seconds 3

$vol = Get-Volume -FileSystemLabel 'UBMCINST' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $vol) {
    $vol = Get-Partition -DiskNumber $DiskNumber | Where-Object DriveLetter | Select-Object -First 1 |
        Get-Volume -ErrorAction SilentlyContinue
}
if (-not $vol -or -not $vol.DriveLetter) { throw 'Could not find the new FAT volume. Format manually and copy the files.' }

$dest = "$($vol.DriveLetter):\"
Write-Host "Copying payload to $dest ..." -ForegroundColor Cyan
foreach ($f in $needed) {
    Copy-Item (Join-Path $SourceDir $f) $dest -Force
    Write-Host "  $f" -ForegroundColor Green
}

Write-Host ''
Write-Host '=== Card ready ===' -ForegroundColor Cyan
Get-ChildItem $dest | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
Write-Host 'Next: power off the ATT-V150, insert this card into the BMC microSD slot (J11),' -ForegroundColor Yellow
Write-Host 'power on, and watch the console. You should see "UBMC-SD-INSTALL".' -ForegroundColor Yellow
