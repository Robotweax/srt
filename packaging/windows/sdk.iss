; SPDX-License-Identifier: MIT
#ifndef SdkRoot
  #error SdkRoot is required
#endif
#ifndef ProductVersion
  #error ProductVersion is required
#endif
[Setup]
AppId=Robotweax.SRT.SDK
AppName=Robotweax SRT SDK
AppVersion={#ProductVersion}
AppPublisher=Robotweax GmbH
DefaultDirName={autopf}\Robotweax-SRT
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ChangesEnvironment=yes
OutputBaseFilename=robotweax-srt-{#ProductVersion}-windows-sdk
Compression=lzma2
SolidCompression=yes
LicenseFile={#SdkRoot}\LICENSE.txt
UninstallDisplayName=Robotweax SRT SDK
[Files]
Source: "{#SdkRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Registry]
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; ValueType: expandsz; ValueName: "ROBOTWEAX_SRT"; ValueData: "{app}"
[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if RegKeyExists(HKLM, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\Robotweax.SRT.SDK_is1')
     or FileExists(ExpandConstant('{app}\unins000.exe')) then
    Result := 'Uninstall the previous Robotweax SRT SDK before installing this candidate.';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Current: String;
begin
  if CurUninstallStep = usUninstall then
    if RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'ROBOTWEAX_SRT', Current) then
      if CompareText(Current, ExpandConstant('{app}')) = 0 then
        RegDeleteValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'ROBOTWEAX_SRT');
end;
