# Writes setupapi.txt, packages.txt and log.txt next to this script.
# Run from an administrator PowerShell:
#   powershell -ExecutionPolicy Bypass -File .\get-3-logs.ps1
$here = $PSScriptRoot

Get-Content C:\Windows\INF\setupapi.dev.log -Tail 400 | Out-File (Join-Path $here "setupapi.txt")

pnputil /enum-drivers | Select-String -Context 0,6 'cbsof' | Out-File (Join-Path $here "packages.txt")

& powershell -ExecutionPolicy Bypass -File D:\dev\cbsof-0.4.5\install-on-chromebook.ps1 -ShowLog |
    Out-File (Join-Path $here "log.txt")

"Done: setupapi.txt, packages.txt and log.txt are in $here"
