# Collects everything needed to diagnose the Chromebook audio driver into ONE
# zip next to this script: cbsof-info-<time>.zip. Copy that zip to the PC.
#
# Run from an administrator PowerShell, in the folder you copied this to:
#   powershell -ExecutionPolicy Bypass -File .\collect-info.ps1

$ErrorActionPreference = "Continue"
$here = $PSScriptRoot
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$dir = Join-Path $here "cbsof-info-$stamp"
New-Item -ItemType Directory -Force $dir | Out-Null

function Save($name, $scriptBlock) {
    try { & $scriptBlock 2>&1 | Out-File (Join-Path $dir $name) -Width 300 }
    catch { "error: $_" | Out-File (Join-Path $dir $name) }
}

Write-Host "Collecting into $dir ..."

# 1. cbsof logs (current and previous start) + which binary is loaded
Save "cbsof-log.txt" {
    Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Enum\CSAUDIO" -ErrorAction SilentlyContinue |
        Where-Object { $_.PSChildName -like "ADSP*" } | Get-ChildItem | ForEach-Object {
            $p = Get-ItemProperty (Join-Path $_.PSPath "Device Parameters") -ErrorAction SilentlyContinue
            "==== device $($_.PSChildName)"
            "CbsofPowerUpPending (crash guard): $($p.CbsofPowerUpPending)"
            "---- previous start"; $p.CbsofLogPrevious
            "---- this start";     $p.CbsofLog
        }
    $svc = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\cbsof" -ErrorAction SilentlyContinue
    "==== cbsof service: Start=$($svc.Start) ImagePath=$($svc.ImagePath)"
    if ($svc.ImagePath) {
        $path = $svc.ImagePath -replace '^\\SystemRoot', $env:SystemRoot
        if (Test-Path $path) { $f = Get-Item $path; "binary: $($f.FullName) $($f.Length) bytes $($f.LastWriteTime)" }
    }
}

# 2. Audio-related devices: status, problem code, driver INF and version
Save "devices.txt" {
    Get-PnpDevice | Where-Object { $_.InstanceId -match 'CSAUDIO|DLGS7219|MX98357A|HDAUDIO|DEV_3198|SWD\\MMDEVAPI' } |
        ForEach-Object {
            $id = $_.InstanceId
            $prob = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode -ErrorAction SilentlyContinue).Data
            $inf  = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
            $ver  = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
            $prov = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverProvider -ErrorAction SilentlyContinue).Data
            "{0,-8} problem={1,-4} {2,-50} {3,-12} {4,-14} {5}" -f $_.Status, $prob, $_.FriendlyName, $inf, $ver, $prov
            "         $id"
        }
}

# 3. cbsof packages in the driver store
Save "packages.txt" { pnputil /enum-drivers | Select-String -Context 1,7 'cbsof|csaudiointcsof' }

# 4. Windows driver-install log (why an install was rejected)
Save "setupapi.txt" { Get-Content "$env:SystemRoot\INF\setupapi.dev.log" -Tail 600 }

# 5. Install progress files and the default playback device
Save "install-progress.txt" {
    Get-ChildItem $here -Recurse -Filter install-progress.txt -ErrorAction SilentlyContinue | ForEach-Object {
        "==== $($_.FullName)"; Get-Content $_.FullName
    }
}
Save "sound-endpoints.txt" {
    Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Format-Table Status, FriendlyName -AutoSize
}

# 6. Newest 3 crash dumps
$dumps = Get-ChildItem "$env:SystemRoot\Minidump\*.dmp" -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 3
foreach ($d in $dumps) { Copy-Item $d.FullName $dir -Force }

# 7. Zip it
$zip = "$dir.zip"
Compress-Archive -Force -Path $dir -DestinationPath $zip
Remove-Item -Recurse -Force $dir
Write-Host ""
Write-Host "Done: $zip"
Write-Host "Copy that one file to the PC."
