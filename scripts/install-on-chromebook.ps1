# Run on the Chromebook, in an elevated PowerShell, from the package folder.
#
#   .\install-on-chromebook.ps1             install the open cbsof driver
#   .\install-on-chromebook.ps1 -ShowLog    show what cbsof reported
#   .\install-on-chromebook.ps1 -Restore    switch the DSP device back to Coolstar's driver
#   .\install-on-chromebook.ps1 -ClearGuard let cbsof try the DSP again after it
#                                           detected a crash and left the DSP off
#
# Only the DSP device (CSAUDIO\ADSP...) changes. Coolstar's bus and codec
# drivers stay installed, and Coolstar's driver stays in the driver store so
# -Restore can put it back. Older cbsof packages are removed from the driver
# store, so a Windows rollback goes to Coolstar's driver, not an old cbsof.
param([switch]$Restore, [switch]$ShowLog, [switch]$ClearGuard)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$hwid = "CSAUDIO\ADSP&CTLR_VEN_8086&CTLR_DEV_3198"
$devcon = Join-Path $here "devcon.exe"

function Show-Log {
    $keys = Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Enum\CSAUDIO" -ErrorAction SilentlyContinue |
            Where-Object { $_.PSChildName -like "ADSP*" } | Get-ChildItem
    foreach ($k in $keys) {
        $p = Get-ItemProperty (Join-Path $k.PSPath "Device Parameters") -ErrorAction SilentlyContinue
        if ($p.CbsofLog) {
            "---- cbsof log ($($k.PSChildName))"
            $p.CbsofLog
        } else {
            "No cbsof log yet for $($k.PSChildName)."
        }
    }
    Get-PnpDevice | Where-Object { $_.InstanceId -like "CSAUDIO\ADSP*" } | Format-Table Status, FriendlyName, InstanceId -AutoSize

    # Which cbsof.sys the service actually loads
    $svc = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\cbsof" -ErrorAction SilentlyContinue
    if ($svc) {
        $path = $svc.ImagePath -replace '^\\SystemRoot', $env:SystemRoot -replace '^System32', "$env:SystemRoot\System32" -replace '^\\\?\?\\', ''
        "cbsof service: Start=$($svc.Start) ImagePath=$($svc.ImagePath)"
        if (Test-Path $path) {
            $f = Get-Item $path
            "loaded binary: $($f.FullName)  $($f.Length) bytes  $($f.LastWriteTime)"
        }
    }
}

function Clear-Guard {
    Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Enum\CSAUDIO" -ErrorAction SilentlyContinue |
        Where-Object { $_.PSChildName -like "ADSP*" } | Get-ChildItem | ForEach-Object {
            Remove-ItemProperty -Path (Join-Path $_.PSPath "Device Parameters") -Name CbsofPowerUpPending -ErrorAction SilentlyContinue
        }
}

# Driver-store packages whose original INF is cbsof.inf, e.g. "oem12.inf"
function Get-CbsofPackages {
    $out = pnputil /enum-drivers | Out-String
    $blocks = $out -split "(\r?\n){2,}"
    foreach ($b in $blocks) {
        if ($b -match "Original Name:\s+cbsof\.inf" -and $b -match "Published Name:\s+(oem\d+\.inf)") {
            $Matches[1]
        }
    }
}

if ($ShowLog) { Show-Log; return }

if ($ClearGuard) {
    Clear-Guard
    "Crash guard cleared. Disable and re-enable the device, or restart, to retry."
    return
}

if ($Restore) {
    $cool = Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository" -Directory -Filter "csaudiointcsof.inf_amd64_*" |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $cool) { throw "Coolstar's csaudiointcsof driver is not in the driver store." }
    & $devcon update (Join-Path $cool.FullName "csaudiointcsof.inf") $hwid
    foreach ($p in @(Get-CbsofPackages)) {
        pnputil /delete-driver $p 2>&1 | Out-Null
    }
    "Restored Coolstar's SOF driver and removed cbsof from the driver store. A restart may be needed."
    return
}

# Test signing must be on (Secure Boot off).
$bcd = bcdedit /enum "{current}" | Out-String
if ($bcd -notmatch "testsigning\s+Yes") {
    "Test signing is OFF. Turn it on, restart, then run this script again:"
    "    bcdedit /set testsigning on"
    "(Secure Boot must be disabled in the firmware setup for this to work.)"
    return
}

# Trust the test certificate.
$cer = Join-Path $here "cbsof-test.cer"
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

Clear-Guard

# Remove every older cbsof package first, so Windows can never fall back to
# one of them, plus the pre-0.4.2 binary in System32\drivers.
foreach ($p in @(Get-CbsofPackages)) {
    pnputil /delete-driver $p /uninstall /force 2>&1 | Out-Null
    "Removed old cbsof package $p from the driver store"
}
$stale = Join-Path $env:SystemRoot "System32\drivers\cbsof.sys"
if (Test-Path $stale) {
    Remove-Item $stale -Force -ErrorAction SilentlyContinue
    if (Test-Path $stale) { "Note: $stale is in use; it is unused from 0.4.2 on." } else { "Removed old $stale" }
}

# Force our driver onto the DSP device (Coolstar's would otherwise outrank it).
& $devcon update (Join-Path $here "cbsof\cbsof.inf") $hwid
if ($LASTEXITCODE -gt 1) { throw "devcon update failed ($LASTEXITCODE)" }

Start-Sleep -Seconds 5
Show-Log
