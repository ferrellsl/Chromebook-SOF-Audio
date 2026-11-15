# Run on the Chromebook, in an elevated PowerShell, from the package folder.
#
#   .\install-on-chromebook.ps1            install the open cbsof DSP driver (bring-up build)
#   .\install-on-chromebook.ps1 -ShowLog   show what cbsof reported
#   .\install-on-chromebook.ps1 -Restore   switch the DSP device back to Coolstar's driver
#
# Only the DSP device (CSAUDIO\ADSP...) changes. Coolstar's bus and codec
# drivers stay installed, and Coolstar's driver stays in the driver store so
# -Restore can put it back.
param([switch]$Restore, [switch]$ShowLog)

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
}

if ($ShowLog) { Show-Log; return }

if ($Restore) {
    $cool = Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository" -Directory -Filter "csaudiointcsof.inf_amd64_*" |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $cool) { throw "Coolstar's csaudiointcsof driver is not in the driver store." }
    & $devcon update (Join-Path $cool.FullName "csaudiointcsof.inf") $hwid
    "Restored Coolstar's SOF driver. A restart may be needed."
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

# Force our driver onto the DSP device (Coolstar's would otherwise outrank it).
& $devcon update (Join-Path $here "cbsof\cbsof.inf") $hwid
if ($LASTEXITCODE -gt 1) { throw "devcon update failed ($LASTEXITCODE)" }

Start-Sleep -Seconds 5
Show-Log
