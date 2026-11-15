# Collects the cbsof diagnostics: log.txt (the driver's own log), setupapi.txt,
# packages.txt and hid.txt. Run from an administrator PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\get-3-logs.ps1
# The files go next to this script, or to the Desktop when it runs from the
# installed copy in Program Files.
$here = $PSScriptRoot
$dest = if ($here -like "$env:ProgramFiles*") { [Environment]::GetFolderPath("Desktop") } else { $here }

Get-Content C:\Windows\INF\setupapi.dev.log -Tail 400 | Out-File (Join-Path $dest "setupapi.txt")

pnputil /enum-drivers | Select-String -Context 0,6 'cbsof' | Out-File (Join-Path $dest "packages.txt")

& powershell -ExecutionPolicy Bypass -File (Join-Path $here "install-on-chromebook.ps1") -ShowLog |
    Out-File (Join-Path $dest "log.txt")

Get-PnpDevice | Where-Object { $_.InstanceId -match '2DCF|7219|DLGS' } | Format-Table Status, Class, FriendlyName, InstanceId -AutoSize | Out-File (Join-Path $dest 'hid.txt') -Width 300

"Done: log.txt, setupapi.txt, packages.txt and hid.txt are in $dest"
