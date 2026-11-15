; Chromebook SOF Audio (open) installer. Built by scripts\package.ps1:
;   ISCC /DAppVersion=x.y.z /DPackageDir=<out\package> /DOutDir=<out\release> installer\cbsof.iss
; The work is done by install-on-chromebook.ps1 -Setup / -Uninstall.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef PackageDir
  #define PackageDir "..\out\package"
#endif
#ifndef OutDir
  #define OutDir "..\out\release"
#endif

[Setup]
AppId={{6E1C2B57-3D0A-4C7E-9B7A-CB50F0A0D7A1}
AppName=Chromebook SOF Audio (open)
AppVersion={#AppVersion}
AppPublisher=Stephen Ferrell
AppPublisherURL=https://github.com/ferrellsl/Chromebook-SOF-Audio
DefaultDirName={autopf}\Chromebook SOF Audio
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0.17763
OutputDir={#OutDir}
OutputBaseFilename=Chromebook-SOF-Audio-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
InfoBeforeFile=before-install.txt
UninstallDisplayName=Chromebook SOF Audio (open) {#AppVersion}
SetupLogging=yes

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[UninstallRun]
Filename: "{sysnative}\WindowsPowerShell\v1.0\powershell.exe"; \
  Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\install-on-chromebook.ps1"" -Uninstall"; \
  Flags: runhidden waituntilterminated; RunOnceId: "RemoveCbsof"

[UninstallDelete]
Type: files; Name: "{app}\install-progress.txt"

[Code]
var
  SetupResult: Integer;

// Before anything is copied: Gemini Lake audio DSP present, Secure Boot off.
function InitializeSetup(): Boolean;
var
  Ps, Check: String;
  Code: Integer;
begin
  Result := True;
  Ps := ExpandConstant('{sysnative}\WindowsPowerShell\v1.0\powershell.exe');
  Check := '-NoProfile -Command "if (-not (Get-PnpDevice -ErrorAction SilentlyContinue | ' +
           'Where-Object { $_.InstanceId -like ''PCI\VEN_8086&DEV_3198*'' })) { exit 2 }; ' +
           '$sb = $false; try { $sb = Confirm-SecureBootUEFI } catch {}; if ($sb) { exit 3 }; exit 0"';
  if Exec(Ps, Check, '', SW_HIDE, ewWaitUntilTerminated, Code) then
  begin
    if Code = 2 then
    begin
      MsgBox('This computer is not a supported Gemini Lake Chromebook: its audio DSP (Intel 8086:3198) was not found.' + #13#10#13#10 + 'Nothing was installed.', mbError, MB_OK);
      Result := False;
    end
    else if Code = 3 then
    begin
      MsgBox('Secure Boot is on. Test-signed drivers cannot load while it is on.' + #13#10#13#10 + 'Turn Secure Boot off in the firmware setup, then run this installer again. Nothing was installed.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Ps, Params, Msg: String;
begin
  if CurStep = ssPostInstall then
  begin
    WizardForm.StatusLabel.Caption := 'Installing the audio drivers (this takes about half a minute)...';
    Ps := ExpandConstant('{sysnative}\WindowsPowerShell\v1.0\powershell.exe');
    Params := '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}') + '\install-on-chromebook.ps1" -Setup';
    if not Exec(Ps, Params, ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, SetupResult) then
      SetupResult := -1;
    case SetupResult of
      0: Msg := '';
      2: Msg := 'This computer is not a supported Gemini Lake Chromebook (no Intel audio DSP 8086:3198 was found). Nothing was changed. You can uninstall this program.';
      3: Msg := 'Secure Boot is on. Test-signed drivers cannot load while it is on. Turn Secure Boot off in the firmware setup, then run this installer again.';
    else
      Msg := 'The driver installation did not finish (code ' + IntToStr(SetupResult) + '). See install-progress.txt in ' + ExpandConstant('{app}') + '.';
    end;
    if Msg <> '' then
      MsgBox(Msg, mbError, MB_OK);
  end;
end;

function NeedRestart(): Boolean;
begin
  Result := (SetupResult = 0);
end;

function UninstallNeedRestart(): Boolean;
begin
  Result := True;
end;
