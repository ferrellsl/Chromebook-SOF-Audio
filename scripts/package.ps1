# Builds the four drivers (Release|x64), signs them with a local test
# certificate, and puts one folder per driver into out\package.
#
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1
#
# The test certificate "Chromebook SOF Audio Test" is created in the
# CurrentUser\My store on first run and exported to out\package\cbsof-test.cer.
param([string]$Configuration = "Release")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "out\package"
$kit = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"
$msb = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -version "[17.0,18.0)" -find "MSBuild\**\Bin\amd64\MSBuild.exe" | Select-Object -First 1

# ---- test certificate -------------------------------------------------------
$certName = "Chromebook SOF Audio Test"
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=$certName" } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$certName" `
        -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy Exportable `
        -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10)
}
New-Item -ItemType Directory -Force $out | Out-Null
Export-Certificate -Cert $cert -FilePath (Join-Path $out "cbsof-test.cer") | Out-Null

# ---- build -------------------------------------------------------------------
$drivers = @(
    @{ Name = "sklhdaudbus"; Sln = "drivers\sklhdaudbus\sklhdaudbus.sln" },
    @{ Name = "da7219";      Sln = "drivers\da7219\da7219.sln" },
    @{ Name = "max98357a";   Sln = "drivers\max98357a\max98357a.sln" },
    @{ Name = "cbsof";       Sln = "drivers\cbsof\cbsof.sln" }
)
foreach ($d in $drivers) {
    Write-Host "== building $($d.Name)"
    & $msb (Join-Path $root $d.Sln) /t:Build /p:Configuration=$Configuration /p:Platform=x64 /p:SignMode=Off /m /nologo /v:m /nodeReuse:false
    if ($LASTEXITCODE -ne 0) { throw "$($d.Name) build failed" }
}

# ---- collect, catalog, sign -------------------------------------------------
function Collect($name, $sysPath, $infPath, $extra) {
    $dir = Join-Path $out $name
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory $dir | Out-Null
    Copy-Item $sysPath, $infPath $dir
    foreach ($e in $extra) { Copy-Item $e $dir }
    & "$kit\..\x86\Inf2Cat.exe" /driver:$dir /os:10_X64 /uselocaltime | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed for $name" }
    Get-ChildItem $dir -Include *.sys, *.cat -Recurse | ForEach-Object {
        & "$kit\signtool.exe" sign /fd SHA256 /sha1 $cert.Thumbprint /s My $_.FullName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "signtool failed on $($_.Name)" }
    }
}

$sys = Get-ChildItem -Path (Join-Path $root "drivers") -Recurse -Filter *.sys |
       Where-Object { $_.FullName -match "\\x64\\$Configuration\\" -or $_.FullName -match "\\build\\x64\\$Configuration\\" }
function FindSys($n) { $sys | Where-Object { $_.Name -eq "$n.sys" } | Sort-Object LastWriteTime -Descending | Select-Object -First 1 }
function FindInf($n) {
    $built = Get-ChildItem -Path (Join-Path $root "drivers") -Recurse -Filter "$n.inf" |
             Where-Object { $_.FullName -match "\\x64\\$Configuration\\" -or $_.FullName -match "\\build\\x64\\$Configuration\\" } |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($built) { return $built.FullName }
    return (Get-ChildItem -Path (Join-Path $root "drivers") -Recurse -Filter "$n.inf" | Select-Object -First 1).FullName
}

foreach ($n in "sklhdaudbus", "da7219", "max98357a") {
    Collect $n (FindSys $n).FullName (FindInf $n) @()
}
Collect "cbsof" (FindSys "cbsof").FullName (FindInf "cbsof") @(
    (Join-Path $root "drivers\cbsof\firmware\sof-glk.ri"),
    (Join-Path $root "drivers\cbsof\firmware\spk.cbtp"),
    (Join-Path $root "drivers\cbsof\firmware\mic.cbtp"),
    (Join-Path $root "drivers\cbsof\firmware\dmic.cbtp"))

Copy-Item (Join-Path $root "scripts\install-on-chromebook.ps1") $out
Copy-Item (Join-Path $root "scripts\get-3-logs.ps1") $out
$lic = Join-Path $out "licenses"
New-Item -ItemType Directory -Force $lic | Out-Null
Copy-Item (Join-Path $root "LICENSE") (Join-Path $lic "LICENSE-cbsof-MIT.txt")
Copy-Item (Join-Path $root "THIRD-PARTY-NOTICES.md") $lic
Copy-Item (Join-Path $root "drivers\sklhdaudbus\LICENSE.txt") (Join-Path $lic "LICENSE-sklhdaudbus-BSD3.txt")
Copy-Item (Join-Path $root "drivers\da7219\LICENSE.txt") (Join-Path $lic "LICENSE-da7219-Apache2.txt")
Copy-Item (Join-Path $root "drivers\max98357a\LICENSE.txt") (Join-Path $lic "LICENSE-max98357a-Apache2.txt")
Copy-Item (Join-Path $root "drivers\cbsof\cbsof\pc\LICENSE-MS-PL.txt") (Join-Path $lic "LICENSE-SimpleAudioSample-MS-PL.txt")
Copy-Item (Join-Path $root "drivers\cbsof\firmware\LICENCE.Intel") (Join-Path $lic "LICENCE-SOF-firmware-Intel.txt")
Remove-Item (Join-Path $out "devcon.exe") -ErrorAction SilentlyContinue   # no longer needed (newdev.dll)
Write-Host "Packages are in $out"

# ---- installer and zip (version from the source INF's DriverVer) -------------
$ver = (Select-String -Path (Join-Path $root "drivers\cbsof\cbsof\cbsof.inf") -Pattern 'DriverVer\s*=\s*[^,]+,(\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (Test-Path $iscc) {
    & $iscc /Q "/DAppVersion=$ver" "/DPackageDir=$out" "/DOutDir=$(Join-Path $root 'out\release')" (Join-Path $root "installer\cbsof.iss")
    if ($LASTEXITCODE -ne 0) { throw "installer build failed" }
}
$zipDir = Join-Path $root "out\release\cbsof-$ver"
Remove-Item $zipDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $zipDir | Out-Null
Copy-Item (Join-Path $out "*") $zipDir -Recurse
Copy-Item (Join-Path $root "scripts\cleanup-safemode.ps1") $zipDir
$zip = Join-Path $root "out\release\cbsof-$ver.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $zipDir -DestinationPath $zip
Write-Host "Release files (version $ver) are in $(Join-Path $root 'out\release')"
