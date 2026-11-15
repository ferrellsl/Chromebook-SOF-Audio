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
        if ($p.CbsofLogPrevious) {
            "---- previous cbsof log (the start before this one) ($($k.PSChildName))"
            $p.CbsofLogPrevious
        }
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

# Every step is appended to install-progress.txt right away, so a crash in the
# middle still leaves a record of how far it got.
$progress = Join-Path $here "install-progress.txt"
function Step($msg) {
    Write-Host $msg
    "$(Get-Date -Format HH:mm:ss) $msg" | Out-File $progress -Append
}

function Get-DspDevice {
    Get-PnpDevice | Where-Object { $_.InstanceId -like "CSAUDIO\ADSP*" } | Select-Object -First 1
}

# Remove all cbsof packages. Only call this while the DSP device is DISABLED:
# otherwise Windows starts another (older) cbsof package on it mid-cleanup.
function Remove-CbsofPackages {
    $out = pnputil /enum-drivers | Out-String
    foreach ($m in ([regex]'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+cbsof\.inf').Matches($out)) {
        $p = $m.Groups[1].Value
        pnputil /delete-driver $p /force 2>&1 | Out-Null
        Step "removed old cbsof package $p"
    }
    $stale = Join-Path $env:SystemRoot "System32\drivers\cbsof.sys"
    if (Test-Path $stale) {
        Remove-Item $stale -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path $stale)) { Step "removed old $stale" }
    }
}

$dsp = Get-DspDevice
if (-not $dsp) { throw "The DSP device (CSAUDIO\ADSP...) was not found. Is Coolstar's sklhdaudbus installed?" }

# Check the tools BEFORE changing anything, so a missing file can never leave
# the DSP device disabled half-way through.
Get-ChildItem $here -Recurse -File | Unblock-File -ErrorAction SilentlyContinue
if (-not (Test-Path $devcon)) {
    "devcon.exe is missing from $here, so nothing was changed."
    "Windows Defender may have quarantined it: Windows Security > Virus & threat protection >"
    "Protection history > restore devcon.exe (Microsoft's driver tool), then run this again."
    if ($dsp.Status -ne "OK") {
        pnputil /enable-device "$($dsp.InstanceId)" | Out-Null
        "(The DSP device was not running, so it has been re-enabled.)"
    }
    return
}
if (-not $Restore -and -not (Test-Path (Join-Path $here "cbsof\cbsof.inf"))) {
    "cbsof\cbsof.inf is missing from $here, so nothing was changed. Extract the whole zip again."
    return
}

if ($Restore) {
    $cool = Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository" -Directory -Filter "csaudiointcsof.inf_amd64_*" |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $cool) { throw "Coolstar's csaudiointcsof driver is not in the driver store." }
    "$(Get-Date) restore" | Out-File $progress
    Step "disabling the DSP device"
    pnputil /disable-device "$($dsp.InstanceId)" | Out-Null
    & $devcon update (Join-Path $cool.FullName "csaudiointcsof.inf") $hwid | Out-Null
    Step "Coolstar driver selected"
    Remove-CbsofPackages
    Step "enabling the DSP device"
    pnputil /enable-device "$($dsp.InstanceId)" | Out-Null
    Step "Restored Coolstar's SOF driver. Restart if there is no sound."
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

"$(Get-Date) install from $here" | Out-File $progress
Clear-Guard

# 1. Disable the DSP device, so no driver can start on it while packages change
Step "disabling the DSP device ($($dsp.FriendlyName))"
pnputil /disable-device "$($dsp.InstanceId)" | Out-Null

# 2. Remove every older cbsof package
Remove-CbsofPackages

# 3. Put the new package on the (still disabled) device. Forced, because
#    Coolstar's signed driver would otherwise outrank a test-signed one.
Step "installing $(Join-Path $here 'cbsof\cbsof.inf')"
$r = & $devcon update (Join-Path $here "cbsof\cbsof.inf") $hwid 2>&1 | Out-String
Step "devcon: $($r.Trim())"
if ($LASTEXITCODE -gt 1) {
    Step "devcon update failed ($LASTEXITCODE); the DSP device stays disabled"
    return
}

# 4. Start it
Step "enabling the DSP device"
pnputil /enable-device "$($dsp.InstanceId)" | Out-Null
Start-Sleep -Seconds 5
Step "done"
Show-Log
