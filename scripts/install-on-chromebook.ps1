# Run on the Chromebook, in an elevated PowerShell, from the package folder.
#
#   .\install-on-chromebook.ps1             install (or update) the open cbsof DSP driver
#   .\install-on-chromebook.ps1 -Setup      full install (used by the installer): also turns
#                                           on test signing, trusts the test certificate and
#                                           adds the bus and codec drivers
#   .\install-on-chromebook.ps1 -Uninstall  remove cbsof; the DSP device goes back to the
#                                           driver it had before (e.g. Coolstar's), if any
#   .\install-on-chromebook.ps1 -ShowLog    show what cbsof reported
#   .\install-on-chromebook.ps1 -Restore    switch the DSP device back to Coolstar's driver
#   .\install-on-chromebook.ps1 -ClearGuard let cbsof try the DSP again after it
#                                           detected a crash and left the DSP off
#
# Only the DSP device (CSAUDIO\ADSP...) is forced onto cbsof. Older cbsof
# packages are removed from the driver store, so a Windows rollback never goes
# to an old cbsof.
#   (-Finish runs once at startup, from a task -Setup registers, when -Setup
#    had to turn test signing on: the swap only works once that is active.)
param([switch]$Restore, [switch]$ShowLog, [switch]$ClearGuard, [switch]$Setup, [switch]$Uninstall, [switch]$Finish)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$hwid = "CSAUDIO\ADSP&CTLR_VEN_8086&CTLR_DEV_3198"

function Show-Log {
    $keys = Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Enum\CSAUDIO" -ErrorAction SilentlyContinue |
            Where-Object { $_.PSChildName -like "ADSP*" } | Get-ChildItem
    foreach ($k in $keys) {
        $p = Get-ItemProperty (Join-Path $k.PSPath "Device Parameters") -ErrorAction SilentlyContinue
        if ($p.CbsofLogPrevious) {
            "---- previous cbsof log (the start before this one) ($($k.PSChildName))"
            $p.CbsofLogPrevious
        }
        if ($p.CbsofJack) {
            "---- jack status: $($p.CbsofJack)"
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

# Force a driver onto the devices with this hardware ID (what "devcon update"
# does), through newdev.dll. Forced, because an installed, properly signed
# driver (such as Coolstar's) outranks a test-signed one.
$newdevSrc = @"
using System;
using System.Runtime.InteropServices;
public static class CbsofNewDev {
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hardwareId,
        string fullInfPath, uint installFlags, out bool rebootRequired);
    // Returns 0 on success, else the Win32 / SetupAPI error, read right after
    // the call (PowerShell would lose it in between).
    public static int Update(string hardwareId, string inf, out bool reboot) {
        if (UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hardwareId, inf, 1, out reboot))   // INSTALLFLAG_FORCE
            return 0;
        int e = Marshal.GetLastWin32Error();
        return e == 0 ? -1 : e;
    }
}
"@
function Update-DriverForced($inf, $id) {
    if (-not ("CbsofNewDev" -as [type])) { Add-Type -TypeDefinition $newdevSrc }
    $reboot = $false
    $err = [CbsofNewDev]::Update($id, $inf, [ref]$reboot)
    [pscustomobject]@{ Ok = ($err -eq 0); Error = ('0x{0:X8}' -f $err); Reboot = $reboot }
}

# Only for Gemini Lake Chromebooks: the audio DSP PCI device must be present.
if (-not (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "PCI\VEN_8086&DEV_3198*" })) {
    "This is not a Gemini Lake (Celeron N4000/N4020/N4100/N4120, Pentium N5000/N5030) Chromebook:"
    "its audio DSP (PCI 8086:3198) was not found. Nothing was changed."
    exit 2
}

Get-ChildItem $here -Recurse -File | Unblock-File -ErrorAction SilentlyContinue
if (-not $Restore -and -not $Uninstall -and -not (Test-Path (Join-Path $here "cbsof\cbsof.inf"))) {
    "cbsof\cbsof.inf is missing from $here, so nothing was changed. Extract the whole zip again."
    exit 2
}

$mode = if ($Setup) { "setup" } elseif ($Uninstall) { "uninstall" } elseif ($Restore) { "restore" } elseif ($Finish) { "finish (after restart)" } else { "install" }
if ($Finish) { "$(Get-Date) $mode" | Out-File $progress -Append } else { "$(Get-Date) $mode from $here" | Out-File $progress }

$taskName = "Chromebook SOF Audio - finish setup"
function Remove-FinishTask {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
}

if ($Finish) {
    # At startup the bus driver may still be creating the DSP device.
    for ($i = 0; $i -lt 90 -and -not (Get-DspDevice); $i++) { Start-Sleep -Seconds 1 }
    Start-Sleep -Seconds 10
}

if ($Setup) {
    # Secure Boot blocks test-signed drivers, so nothing below would help.
    $sb = $false
    try { $sb = Confirm-SecureBootUEFI } catch { $sb = $false }
    if ($sb) {
        Step "Secure Boot is ON. Turn it off in the firmware setup, then run the installer again. Nothing was changed."
        exit 3
    }
    $bcd = bcdedit /enum "{current}" | Out-String
    $deferSwap = $false
    if ($bcd -notmatch "testsigning\s+Yes") {
        bcdedit /set testsigning on | Out-Null
        Step "test signing turned on (takes effect after the restart)"
        $deferSwap = $true
    }
    $cer = Join-Path $here "cbsof-test.cer"
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Step "test certificate trusted"
    # The bus and codec drivers (Coolstar's open-source drivers, built from
    # source). Where Coolstar's own signed copies are installed, Windows keeps those.
    foreach ($n in "sklhdaudbus", "da7219", "max98357a") {
        $inf = Join-Path $here "$n\$n.inf"
        pnputil /add-driver $inf /install 2>&1 | Out-Null
        Step "$n driver added (pnputil exit $LASTEXITCODE)"
    }
    if ($deferSwap) {
        # A test-signed driver cannot take over the DSP device until test
        # signing is active, i.e. after the restart. Stage it now and let a
        # one-time startup task (as SYSTEM) do the swap.
        pnputil /add-driver (Join-Path $here "cbsof\cbsof.inf") 2>&1 | Out-Null
        $ps = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
        $script = Join-Path $here "install-on-chromebook.ps1"
        Remove-FinishTask
        $action = New-ScheduledTaskAction -Execute $ps -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$script`" -Finish"
        $trigger = New-ScheduledTaskTrigger -AtStartup
        $principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
        Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Force | Out-Null
        Step "cbsof staged; it takes over the DSP device at the next startup"
        exit 0
    }
} elseif (-not $Uninstall -and -not $Restore -and -not $Finish) {
    $bcd = bcdedit /enum "{current}" | Out-String
    if ($bcd -notmatch "testsigning\s+Yes") {
        "Test signing is OFF. Turn it on, restart, then run this script again:"
        "    bcdedit /set testsigning on"
        "(Secure Boot must be disabled in the firmware setup for this to work.)"
        exit 3
    }
    $cer = Join-Path $here "cbsof-test.cer"
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
}

$dsp = Get-DspDevice
if (-not $dsp) {
    if ($Uninstall -or $Restore) {
        Step "The DSP device (CSAUDIO\ADSP...) was not found."
        if ($Uninstall) {
            $out = pnputil /enum-drivers | Out-String
            foreach ($m in ([regex]'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+cbsof\.inf').Matches($out)) {
                pnputil /delete-driver $m.Groups[1].Value /force 2>&1 | Out-Null
            }
            Step "cbsof packages removed"
            Remove-FinishTask
            exit 0
        }
        exit 2
    }
    # The bus driver creates the DSP device. On a machine that had no audio
    # drivers yet it appears after the restart, and gets cbsof then.
    pnputil /add-driver (Join-Path $here "cbsof\cbsof.inf") /install 2>&1 | Out-Null
    Step "cbsof added; the DSP device appears after the restart and will use it"
    exit 0
}

if ($Uninstall) {
    Remove-FinishTask
    Step "disabling the DSP device"
    pnputil /disable-device "$($dsp.InstanceId)" | Out-Null
    Remove-CbsofPackages
    pnputil /enable-device "$($dsp.InstanceId)" | Out-Null
    Step "cbsof removed; after the restart the DSP device uses its previous driver, if there is one"
    exit 0
}

if ($Restore) {
    $cool = Get-ChildItem "$env:SystemRoot\System32\DriverStore\FileRepository" -Directory -Filter "csaudiointcsof.inf_amd64_*" |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $cool) { throw "Coolstar's csaudiointcsof driver is not in the driver store." }
    Step "disabling the DSP device"
    pnputil /disable-device "$($dsp.InstanceId)" | Out-Null
    Remove-CbsofPackages
    Step "enabling the DSP device"
    pnputil /enable-device "$($dsp.InstanceId)" | Out-Null
    Start-Sleep -Seconds 3
    # Coolstar's is now the only driver for it; make sure it is the one in use.
    $u = Update-DriverForced (Join-Path $cool.FullName "csaudiointcsof.inf") $hwid
    Step "driver update: ok=$($u.Ok) error=$($u.Error)"
    Step "Restored Coolstar's SOF driver. Restart if there is no sound."
    exit 0
}

Clear-Guard

# 1. Disable the DSP device, so no driver can start on it while packages change
Step "disabling the DSP device ($($dsp.FriendlyName))"
pnputil /disable-device "$($dsp.InstanceId)" | Out-Null

# 2. Remove every older cbsof package, so nothing old is left to fall back to
Remove-CbsofPackages

# 3. Re-enable. Until step 4 its previous driver (e.g. Coolstar's) runs it, or
#    none; that is expected.
Step "enabling the DSP device (its previous driver runs until the next step)"
pnputil /enable-device "$($dsp.InstanceId)" | Out-Null
Start-Sleep -Seconds 3

# 4. Swap it to cbsof, forced. (A disabled device cannot be updated, which is
#    why step 3 comes first.)
Step "installing $(Join-Path $here 'cbsof\cbsof.inf')"
$u = Update-DriverForced (Join-Path $here "cbsof\cbsof.inf") $hwid
Step "driver update: ok=$($u.Ok) error=$($u.Error) reboot=$($u.Reboot)"
if ($Finish) { Remove-FinishTask }
if (-not $u.Ok) {
    Step "installing cbsof failed; the DSP device stays on its previous driver"
    exit 4
}
Start-Sleep -Seconds 5
Step "done. Restart Windows to finish."
if (-not $Setup) { Show-Log }
exit 0
