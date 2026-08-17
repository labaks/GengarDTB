# Build, flash and capture the boot log in one pass.
#
#   powershell -ExecutionPolicy Bypass -File tools\flash.ps1          # flash + 15 s of log
#   powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Log 30  # longer capture
#   powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -NoLog   # flash only
param(
    [int]$Log = 15,
    [switch]$NoLog
)

$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\find-port.ps1"

$port = Get-CydPort
Write-Output "port: $port"

& 'C:\esp\esp-idf\export.ps1' | Out-Null
Set-Location (Split-Path $PSScriptRoot -Parent)

idf.py -p $port -b 460800 flash
if ($LASTEXITCODE -ne 0) {
    Write-Output "flash FAILED ($LASTEXITCODE)"
    Write-Output "If this says 'No serial data received': something left RTS asserted and"
    Write-Output "the chip is held in reset. Replug USB, or hold BOOT and tap RST."
    exit 1
}

if (-not $NoLog) {
    # Let the port settle: flashing and reading back to back trips 'port is busy'.
    Start-Sleep -Milliseconds 400
    python "$PSScriptRoot\monitor.py" $Log $port
}
