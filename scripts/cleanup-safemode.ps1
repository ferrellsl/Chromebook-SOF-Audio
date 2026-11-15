# Removes every cbsof driver package from this Chromebook so Windows goes back
# to Coolstar's SOF driver. Run it in SAFE MODE, from an administrator
# PowerShell, in the folder you copied it to:
#
#   powershell -ExecutionPolicy Bypass -File .\cleanup-safemode.ps1
#
# It also copies the newest crash dumps next to this script and writes a log
# (cleanup-log.txt) there. Restart normally afterwards.

$ErrorActionPreference = "Continue"
$here = $PSScriptRoot
$log = Join-Path $here "cleanup-log.txt"
"cbsof cleanup $(Get-Date)" | Out-File $log

function Say($msg) {
    Write-Host $msg
    $msg | Out-File $log -Append
}

# Must be elevated
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Say "Please run this from an administrator PowerShell (right-click > Run as administrator)."
    return
}

$safe = (Get-CimInstance Win32_ComputerSystem).BootupState
Say "Boot mode: $safe"
if ($safe -notmatch "Fail-safe") {
    Say "WARNING: this does not look like Safe Mode. It is safest to run this in Safe Mode."
}

# 1. Keep the cbsof service from starting while we work
if (Test-Path "HKLM:\SYSTEM\CurrentControlSet\Services\cbsof") {
    Set-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\cbsof" -Name Start -Value 4
    Say "cbsof service disabled (Start=4)"
}

# 2. Delete every driver-store package whose original INF is cbsof.inf
$out = pnputil /enum-drivers | Out-String
$found = ([regex]'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+cbsof\.inf').Matches($out)
if ($found.Count -eq 0) {
    Say "No cbsof packages in the driver store."
}
foreach ($m in $found) {
    $p = $m.Groups[1].Value
    $r = pnputil /delete-driver $p /force 2>&1 | Out-String
    Say "Deleting $p : $($r.Trim())"
}
$left = pnputil /enum-drivers | Select-String -SimpleMatch "cbsof.inf"
if ($left) {
    Say "WARNING: cbsof packages still present:"
    $left | ForEach-Object { Say "  $_" }
} else {
    Say "Driver store is clean: no cbsof packages left."
}

# 3. Old binary from before 0.4.2
$stale = Join-Path $env:SystemRoot "System32\drivers\cbsof.sys"
if (Test-Path $stale) {
    Remove-Item $stale -Force -ErrorAction SilentlyContinue
    if (Test-Path $stale) { Say "Could not delete $stale (in use?)" } else { Say "Deleted $stale" }
}

# 4. Crash dumps for analysis (newest 5)
$dumps = Get-ChildItem "$env:SystemRoot\Minidump\*.dmp" -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 5
foreach ($d in $dumps) {
    Copy-Item $d.FullName $here -Force
    Say "Copied dump $($d.Name) ($($d.LastWriteTime))"
}
if (-not $dumps) { Say "No minidumps found in $env:SystemRoot\Minidump" }

# 5. Current state of the DSP device
Get-PnpDevice | Where-Object { $_.InstanceId -like "CSAUDIO\ADSP*" } | ForEach-Object {
    Say "DSP device: $($_.Status)  $($_.FriendlyName)"
}

Say ""
Say "Done. Restart normally. Windows should put Coolstar's driver back on the DSP device."
Say "Then copy this folder (cleanup-log.txt and the .dmp files) back to the PC."
