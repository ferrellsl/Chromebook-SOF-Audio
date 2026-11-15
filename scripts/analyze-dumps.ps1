# Analyzes the newest crash dumps ON THE CHROMEBOOK with the bundled Windows
# debugger, and writes one text file per dump next to this script. Send the
# .txt files back; they are small.
#
#   powershell -ExecutionPolicy Bypass -File .\analyze-dumps.ps1
#
# Needs internet for Microsoft's symbol server (Windows symbols only; nothing
# is uploaded). cbsof.pdb in this folder supplies our driver's symbols.
param([int]$Count = 2)

$here = $PSScriptRoot
$kd = Join-Path $here "debugger\kd.exe"
if (-not (Test-Path $kd)) { throw "debugger\kd.exe is missing from $here" }

$cache = Join-Path $here "symcache"
New-Item -ItemType Directory -Force $cache | Out-Null
$sym = "srv*$cache*https://msdl.microsoft.com/download/symbols;$here"

$dumps = @(Get-ChildItem "$env:SystemRoot\Minidump\*.dmp" -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First $Count)
if (-not $dumps) { "No dumps in $env:SystemRoot\Minidump"; return }

foreach ($d in $dumps) {
    $out = Join-Path $here ("analysis-" + $d.BaseName + ".txt")
    "Analyzing $($d.Name) ($($d.LastWriteTime)) -> $out"
    $cmds = ".bugcheck; lmvm cbsof; kn 40; .ecxr; r; !analyze -v; q"
    & $kd -z $d.FullName -y $sym -i $here -c $cmds -logo $out | Out-Null
}
"Done. Send the analysis-*.txt files."
