#Requires -Version 5.1
<#
.SYNOPSIS
    Press a rotating set of firmware-setup keys on the console port, with a
    delay so the keys land in the BIOS window rather than during BMC init.

.DESCRIPTION
    The ATT-V150 boot timeline (measured) looks like this:

        T+0s    BMC SPL banner "Initializing uBMC Firmware. Version UBMC_R102_..."
        T+0-45s BMC userspace coming up; the host sits in "Waiting for BMC"
        T+45s   BlinkBoot lenses (Usb/Sata/SdMmc) validate and install
        T+52s   GRUB menu

    So the firmware setup key has to be pressed somewhere in T+35..T+75.
    One key is not enough - Insyde/BlinkBoot builds use different ones
    depending on the terminal type - so this cycles ESC, DEL, F2 and F10.

.PARAMETER Port
    Serial port. Omit to auto-pick when exactly one port is present.

.PARAMETER ListPorts
    Print the detected serial ports and exit.

.PARAMETER DelaySeconds
    Wait this long before sending anything (covers BMC init).

.PARAMETER PressSeconds
    How long to keep cycling keys.

.PARAMETER QuietSeconds
    Then keep listening this long to capture whatever the firmware printed.

.EXAMPLE
    .\press-setup-keys.ps1 -ListPorts

.EXAMPLE
    .\press-setup-keys.ps1 -DelaySeconds 35
#>
[CmdletBinding()]
param(
    [string]$Port,
    [int]$Baud = 115200,
    [int]$DelaySeconds = 35,
    [int]$PressSeconds = 45,
    [int]$QuietSeconds = 30,
    [switch]$ListPorts
)

$ErrorActionPreference = 'Stop'

function Get-PortList { @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object) }

if ($ListPorts) {
    $p = Get-PortList
    if ($p.Count -eq 0) { Write-Host 'No serial port detected.' -ForegroundColor Yellow; exit 1 }
    Write-Host 'Detected serial ports:'
    $p | ForEach-Object { Write-Host "  $_" }
    exit 0
}

if (-not $Port) {
    $p = Get-PortList
    if ($p.Count -eq 0) {
        Write-Host 'No serial port detected. Plug the console cable in first,' -ForegroundColor Red
        Write-Host 'or pass -Port COMx explicitly.' -ForegroundColor Red
        exit 1
    }
    if ($p.Count -gt 1) {
        Write-Host 'More than one serial port found - pick one with -Port:' -ForegroundColor Yellow
        $p | ForEach-Object { Write-Host "  $_" }
        exit 1
    }
    $Port = $p[0]
}

$logPath = Join-Path $PSScriptRoot ("setupkeys-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')

$esc = [string][char]27
$sequence = @(
    $esc,                                            # ESC  - Insyde "Enter Setup"
    [string][char]127,                               # DEL
    "$esc" + 'OQ',                                   # F2   (xterm)
    "$esc" + '[12~'                                  # F2   (vt100+)
)

$serial = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$serial.Handshake = [System.IO.Ports.Handshake]::None
$serial.ReadTimeout = 150
$serial.DtrEnable = $false
$serial.RtsEnable = $false
try { $serial.Open() } catch { throw "Cannot open $Port : $($_.Exception.Message)" }

$log = New-Object System.Text.StringBuilder
function Emit([string]$text) {
    [void]$log.Append($text)
    Write-Host ("[{0:HH:mm:ss.fff}] " -f (Get-Date)) -NoNewline -ForegroundColor DarkGray
    Write-Host $text -NoNewline
}

Write-Host ("Port {0} open. POWER THE APPLIANCE ON NOW." -f $Port) -ForegroundColor Cyan
Write-Host ("Silent for {0}s, then cycling setup keys for {1}s, then listening {2}s." -f $DelaySeconds, $PressSeconds, $QuietSeconds) -ForegroundColor DarkGray
Write-Host ''

$start = Get-Date
$keyIndex = 0
$lastKey = [datetime]::MinValue
$sawIt = $false

try {
    while (((Get-Date) - $start).TotalSeconds -lt ($DelaySeconds + $PressSeconds + $QuietSeconds)) {
        $elapsed = ((Get-Date) - $start).TotalSeconds

        if ($elapsed -ge $DelaySeconds -and $elapsed -lt ($DelaySeconds + $PressSeconds)) {
            if (((Get-Date) - $lastKey).TotalMilliseconds -ge 250) {
                try { $serial.Write($sequence[$keyIndex % $sequence.Count]) } catch { }
                $keyIndex++
                $lastKey = Get-Date
            }
        }

        try { $chunk = $serial.ReadExisting() } catch { $chunk = '' }
        if (-not $chunk) { Start-Sleep -Milliseconds 30; continue }

        Emit $chunk

        if ($chunk -match 'Setup|BIOS|Insyde|Main Menu|Advanced|Server Mgmt|Save & Exit') { $sawIt = $true }
    }
}
finally { try { $serial.Close() } catch { } }

Set-Content -Path $logPath -Value $log.ToString() -Encoding UTF8
Write-Host ''
Write-Host ''
if ($sawIt) {
    Write-Host '*** Setup UI keywords detected - we are in the firmware menu. ***' -ForegroundColor Green
}
else {
    Write-Host 'No setup UI seen. Either the key window was missed, or this firmware' -ForegroundColor Yellow
    Write-Host 'does not expose setup on the serial console.' -ForegroundColor Yellow
}
Write-Host ("Log: {0}" -f (Split-Path $logPath -Leaf)) -ForegroundColor DarkGray
