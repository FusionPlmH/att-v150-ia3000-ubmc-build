#Requires -Version 5.1
<#
.SYNOPSIS
    串口抓取 / 发送工具（替代原来的 Console-Expect / Console-Run / Watch-Console 三个脚本）。

.DESCRIPTION
    这台机器没有显卡输出，所有操作必须走串口。这个脚本做三件事：
      1. 实时打印串口输出，同时写日志文件
      2. 往串口发命令（例如给静默的 U-Boot 按回车、驱动 GRUB 菜单）
      3. 限时抓取（抓启动过程，到点自动停）

.PARAMETER Port
    串口号。不指定时自动使用唯一存在的串口；有多个则列出来让你选。

.PARAMETER ListPorts
    只列出检测到的串口然后退出。

.PARAMETER Baud
    默认 115200（本机固定 8N1，无流控）。

.PARAMETER Seconds
    抓多少秒后自动停。0 = 一直抓到 Ctrl-C。

.PARAMETER Send
    打开串口后发一串文本。'\r' '\n' '\t' 会被解释成回车/换行/制表。
    例如 -Send "reset\r"

.PARAMETER LogFile
    日志路径。不指定则写到当前目录 serial-<时间戳>.log。

.PARAMETER StripAnsi
    写日志时去掉 ANSI 转义序列（GRUB 和 systemd 会刷一堆，去掉后好读很多）。

.EXAMPLE
    .\serial-console.ps1 -ListPorts
    看看这台机器上有哪些串口。

.EXAMPLE
    .\serial-console.ps1 -Seconds 30
    抓 30 秒启动输出。

.EXAMPLE
    .\serial-console.ps1 -Send "reset\r" -Seconds 20
    给静默的 U-Boot 发 reset，然后抓 20 秒看重启过程。

.EXAMPLE
    .\serial-console.ps1 -Send "`e" -Seconds 5
    往 GRUB 菜单发 ESC（退出编辑回到菜单）。
#>
param(
    [string]$Port,
    [int]$Baud = 115200,
    [int]$Seconds = 0,
    [string]$Send,
    [string]$LogFile,
    [switch]$StripAnsi,
    [switch]$ListPorts
)

$ErrorActionPreference = 'Stop'

function Get-PortList { @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object) }

if ($ListPorts) {
    $p = Get-PortList
    if ($p.Count -eq 0) { Write-Host '没有检测到串口。' -ForegroundColor Yellow; exit 1 }
    Write-Host '检测到的串口：'
    $p | ForEach-Object { Write-Host "  $_" }
    exit 0
}

if (-not $Port) {
    $p = Get-PortList
    if ($p.Count -eq 0) {
        Write-Host '没有检测到串口。先把 console 线插上，' -ForegroundColor Red
        Write-Host '或者用 -Port COMx 明确指定。' -ForegroundColor Red
        exit 1
    }
    if ($p.Count -gt 1) {
        Write-Host '检测到多个串口，用 -Port 指定其中一个：' -ForegroundColor Yellow
        $p | ForEach-Object { Write-Host "  $_" }
        exit 1
    }
    $Port = $p[0]
}

if (-not $LogFile) {
    $LogFile = Join-Path (Get-Location) ("serial-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}

function Clean([string]$s) {
    # ANSI CSI 和简单 ESC 序列
    $s = [regex]::Replace($s, "$([char]27)\[[0-9;?]*[a-zA-Z]", '')
    $s = [regex]::Replace($s, "$([char]27)[()][A-Z0-9]", '')
    return $s
}

$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, 'None', 8, 'One')
$sp.ReadTimeout = 500
$sp.WriteTimeout = 2000

try {
    $sp.Open()
}
catch {
    Write-Host "打不开 $Port ：$($_.Exception.Message)" -ForegroundColor Red
    Write-Host '用设备管理器或 [System.IO.Ports.SerialPort]::GetPortNames() 确认端口号。'
    exit 1
}

Write-Host "== 串口 $Port @ $Baud 8N1" -ForegroundColor Cyan
Write-Host "== 日志 $LogFile" -ForegroundColor Cyan
if ($Seconds -gt 0) { Write-Host "== 抓 $Seconds 秒" -ForegroundColor Cyan }
else { Write-Host '== 一直抓，Ctrl-C 结束' -ForegroundColor Cyan }
Write-Host ''

$writer = New-Object System.IO.StreamWriter($LogFile, $true, [System.Text.Encoding]::UTF8)
$writer.AutoFlush = $true

try {
    if ($Send) {
        $text = $Send -replace '\\r', "`r" -replace '\\n', "`n" -replace '\\t', "`t" -replace '\\e', "`e"
        $sp.Write($text)
        Write-Host ">> 已发送 $($text.Length) 字符" -ForegroundColor Yellow
        Write-Host ''
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        if ($Seconds -gt 0 -and $sw.Elapsed.TotalSeconds -ge $Seconds) { break }

        try {
            $chunk = $sp.ReadExisting()
        }
        catch {
            $chunk = ''
        }

        if ($chunk) {
            $out = if ($StripAnsi) { Clean $chunk } else { $chunk }
            Write-Host $out -NoNewline
            $writer.Write($out)
        }
        else {
            Start-Sleep -Milliseconds 100
        }
    }
}
finally {
    $writer.Close()
    $sp.Close()
    $sp.Dispose()
    Write-Host ''
    Write-Host "== 结束，日志在 $LogFile" -ForegroundColor Cyan
}
